#pragma once
///@file

#include "nix/util/serialise.hh"
#include "nix/store/store-api.hh"

#include <functional>

namespace nix {

struct Builder;

namespace daemon {

enum struct RecursiveFlag {
    NotRecursive = 0,
    Recursive = 1,
    RecursiveSubmitted = 2,
};

/**
 * Serve a client on the given file descriptors.
 *
 * @param setupTelemetry Called once after the handshake and logger
 * setup, with the W3C `traceparent` string received from the client
 * (empty if the client sent none or the protocol feature was not
 * negotiated). Allows the caller to set up distributed tracing for
 * the connection, e.g. by adding a tracing logger.
 *
 * @param builder The builder to use for build operations. If null,
 * one is obtained from `store->getBuilder()`.
 */
void processConnection(
    ref<Store> store,
    FdSource && from,
    FdSink && to,
    TrustedFlag trusted,
    RecursiveFlag recursive,
    std::function<void(std::string_view traceparent)> setupTelemetry = {},
    std::shared_ptr<Builder> builder = nullptr);

} // namespace daemon

} // namespace nix
