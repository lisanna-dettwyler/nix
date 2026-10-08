#pragma once
///@file

#include "nix/store/store-api.hh"
#include "nix/util/file-descriptor.hh"
#include "nix/util/serialise.hh"

#include <functional>

namespace nix {

/**
 * The build hook: answer requests from `from` on `toParent` and run
 * an accepted build on a machine from `builders`. Returns at EOF or
 * after one accepted build.
 *
 * @param maxBuildJobs the parent's `max-jobs`.
 * @param sshErrorFd ssh's stderr, read when a connection fails.
 * @param onMachineSelected called once, just before the first
 *   connection to a remote builder is attempted (i.e. once the hook
 *   knows it is actually going to be used). The `nix __build-remote`
 *   command uses it to set up tracing lazily.
 */
void serveBuildHook(
    ref<Store> store,
    unsigned int maxBuildJobs,
    Source & from,
    Descriptor toParent,
    Descriptor sshErrorFd,
    std::function<void()> onMachineSelected = {});

} // namespace nix
