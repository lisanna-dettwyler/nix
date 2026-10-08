#include <cstdlib>

#include "nix/store/build/build-remote.hh"
#include "nix/main/shared.hh"
#include "nix/main/plugin.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/util/config-global.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/serialise.hh"
#include "nix/util/util.hh"
#include "nix/cmd/legacy.hh"

#include "otel-logger.hh"

namespace nix {

static int main_build_remote(int argc, char ** argv)
{
    {
        /* Upon exiting, Nix will attempt to terminate this process with
           SIGTERM. initNix will block or handle SIGTERM, so we need to unblock
           and unhandle it here.
        */
        struct sigaction act;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;
        act.sa_handler = SIG_DFL;
        if (sigaction(SIGTERM, &act, 0))
            throw SysError("resetting SIGTERM");

        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGTERM);
        if (pthread_sigmask(SIG_UNBLOCK, &set, nullptr))
            throw SysError("unblocking SIGTERM");

        logger = makeJSONLogger(getStandardError()).release();

        /* Ensure we don't get any SSH passphrase or host key popups. */
        unsetenv("DISPLAY");
        unsetenv("SSH_ASKPASS");

        /* If we ever use the common args framework, make sure to
           remove initPlugins below and initialize settings first.
        */
        if (argc != 2)
            throw UsageError("called without required arguments");

        verbosity = (Verbosity) std::stoll(argv[1]);

        FdSource source(STDIN_FILENO);

        /* Read the parent's settings. The parent sends the settings of
           all `Config`s, not just `Settings`, so apply them
           accordingly. Only apply those that differ from ours, though:
           we've read the same configuration files, so the rest are
           defaults, and setting a deprecated setting warns even if
           it's to its default. (Note that we can't just have the
           parent send its overridden settings: things like `--store`
           set the setting without marking it as overridden.) */
        std::map<std::string, Config::SettingInfo> ourSettings;
        globalConfig.getSettings(ourSettings);
        while (readInt(source)) {
            auto name = readString(source);
            auto value = readString(source);
            if (auto i = ourSettings.find(name); i == ourSettings.end() || i->second.value != value)
                globalConfig.set(name, value);
        }

        unsigned int maxBuildJobs = settings.getWorkerSettings().maxBuildJobs.get();
        settings.getWorkerSettings().maxBuildJobs.set("1"); // hack to make tests with local?root= work

        initPlugins();

        auto store = openStore();

        serveBuildHook(store, maxBuildJobs, source, STDERR_FILENO, 5, [] {
            /* Set up tracing only now that we're actually going to
               use a remote builder, so that a build hook that
               turns out to be unused (which is the common case,
               since we're started for every build) doesn't upload
               a trace. Our root span is parented to the trace of
               the `nix` process that runs us, which it passes in
               `TRACEPARENT`. */
            initOtel("nix-build-remote", "nix-build-remote", getEnv("TRACEPARENT").value_or(""));
        });

        return 0;
    }
}

static RegisterLegacyCommand r_build_remote("build-remote", main_build_remote);

} // namespace nix
