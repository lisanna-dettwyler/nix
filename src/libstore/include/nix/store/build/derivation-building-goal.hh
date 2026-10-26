#pragma once
///@file

#include "nix/store/derivations.hh"
#include "nix/store/local-store.hh"
#include "nix/store/parsed-derivations.hh"
#include "nix/store/derivation-options.hh"
#include "nix/store/build/derivation-building-misc.hh"
#include "nix/store/store-api.hh"
#include "nix/store/pathlocks.hh"
#include "nix/store/build/goal.hh"
#include "nix/store/build/build-log.hh"
#include "nix/store/machine-selection.hh"

namespace nix {

using std::map;

struct BuilderFailureError;
struct ExternalBuilder;
#ifndef _WIN32 // TODO enable build hook on Windows
struct HookInstance;
struct DerivationBuilder;
#endif

typedef enum { rpAccept, rpDecline, rpPostpone } HookReply;

/**
 * A goal for building a derivation. Substitution, (or any other method of
 * obtaining the outputs) will not be attempted, so it is the calling goal's
 * responsibility to try to substitute first.
 */
struct DerivationBuildingGoal : public Goal
{
    friend class Worker;

    /**
     * @param storeDerivation Whether to store the derivation in
     * `worker.store`. This is useful for newly-resolved derivations. In this
     * case, the derivation was not created a priori, e.g. purely (or close
     * enough) from evaluation of the Nix language, but also depends on the
     * exact content produced by upstream builds. It is strongly advised to
     * have a permanent record of such a resolved derivation in order to
     * faithfully reconstruct the build history.
     */
    DerivationBuildingGoal(
        const StorePath & drvPath,
        ref<const Derivation> drv,
        Worker & worker,
        BuildMode buildMode,
        bool storeDerivation);
    ~DerivationBuildingGoal();

private:

    /** The path of the derivation. */
    const StorePath drvPath;

    /**
     * The derivation stored at drvPath.
     */
    const ref<const Derivation> drv;

    /**
     * The remainder is state held during the build.
     */

    const BuildMode buildMode;

    std::unique_ptr<MaintainCount<uint64_t>> mcRunningBuilds;

    std::string key() override;

    struct LocalBuildCapability
    {
        LocalStore & localStore;
        const ExternalBuilder * externalBuilder;
    };

    /**
     * The states.
     */
    Co gaveUpOnSubstitution(bool storeDerivation);
    Co tryToBuild(StorePathSet inputPaths);
    Co buildWithHook(
        StorePathSet inputPaths,
        std::map<std::string, InitialOutput> initialOutputs,
        DerivationOptions<StorePath> drvOptions,
        PathLocks outputLocks);
    Co buildLocally(
        LocalBuildCapability localBuildCap,
        StorePathSet inputPaths,
        std::map<std::string, InitialOutput> initialOutputs,
        DerivationOptions<StorePath> drvOptions,
        PathLocks outputLocks);

    /**
     * Is the build hook willing to perform the build?
     */
    Co tryBuildHook(const DerivationOptions<StorePath> & drvOptions, HookReply & reply);

#ifndef _WIN32
    std::unique_ptr<HookInstance> hookAccepted;
    std::string hookMachineName;

    /** Skipped by this build only. Shared with the coroutine, which can outlive the goal. */
    std::shared_ptr<std::set<const Machine *>> unreachableMachines = std::make_shared<std::set<const Machine *>>();
#endif

    /** `machine` is the reserved slot, empty when the hook program answered. */
    struct RemoteReply
    {
        HookReply reply = rpDecline;
        std::optional<AcquiredMachine> machine;
    };

    /** Asks the hook program if `build-hook` is not the default, else the worker's own remote builder. */
    Co tryRemoteBuilder(const DerivationOptions<StorePath> & drvOptions, RemoteReply & reply);

#ifndef _WIN32
    RemoteReply selectRemoteMachine(const DerivationOptions<StorePath> & drvOptions);

    /**
     * Build on `machine` in this process. Ends the goal, except when no
     * machine could be reached: then it sets `retry` and returns.
     */
    Co buildRemotely(
        AcquiredMachine machine,
        const DerivationOptions<StorePath> & drvOptions,
        const StorePathSet & inputPaths,
        std::map<std::string, InitialOutput> & initialOutputs,
        PathLocks outputLocks,
        bool & retry);
#endif

    Done doneFailureLogTooLong(BuildLog & buildLog);

    /**
     * Wrappers around the corresponding Store methods that first consult the
     * derivation.  This is currently needed because when there is no drv file
     * there also is no DB entry.
     */
    std::map<std::string, std::optional<StorePath>> queryPartialDerivationOutputMap();

    /**
     * Update 'initialOutputs' to determine the current status of the
     * outputs of the derivation. Also returns a Boolean denoting
     * whether all outputs are valid and non-corrupt, and a
     * 'SingleDrvOutputs' structure containing the valid outputs.
     */
    std::pair<bool, SingleDrvOutputs> checkPathValidity(std::map<std::string, InitialOutput> & initialOutputs);

    Done doneSuccess(
        BuildResult::Success::Status status,
        SingleDrvOutputs builtOutputs,
        ActivityId act = 0,
        std::shared_ptr<const Provenance> provenance = nullptr);

    Done doneFailure(BuildError ex, ActivityId act = 0);

    BuildError fixupBuilderFailureErrorMessage(BuilderFailureError msg, BuildLog & buildLog);

    JobCategory jobCategory() const override
    {
        return JobCategory::Build;
    };
};

} // namespace nix
