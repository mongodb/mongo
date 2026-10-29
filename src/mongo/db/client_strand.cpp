// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/client_strand.h"

#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/decorable.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {
// Rely on Decorable zeroing this memory to take advantage of a
// trivially_constructable optimization in the decoration code.
struct ClientStrandData {
    ClientStrand* strand;
};

auto getClientStrandData = Client::declareDecoration<ClientStrandData>();
}  // namespace

boost::intrusive_ptr<ClientStrand> ClientStrand::make(ServiceContext::UniqueClient client) {
    auto strand = make_intrusive<ClientStrand>(std::move(client));
    getClientStrandData(strand->getClientPointer()).strand = strand.get();
    return strand;
}

boost::intrusive_ptr<ClientStrand> ClientStrand::get(Client* client) {
    return getClientStrandData(client).strand;
}

void ClientStrand::_setCurrent() {
    invariant(_isBound.load());
    invariant(_client);

    LOGV2_DEBUG(
        5127801, kDiagnosticLogLevel, "Setting the Client", "client"_attr = _client->desc());

    // Set the Client for this thread so calls to Client::getCurrent() works as expected.
    Client::setCurrent(std::move(_client));

    // Set up the thread name.
    _oldThreadName = setThreadNameRef(_threadName);
    if (_oldThreadName) {
        LOGV2_DEBUG(5127802, kDiagnosticLogLevel, "Set thread name", "name"_attr = *_threadName);
    }
}

void ClientStrand::_releaseCurrent() {
    invariant(_isBound.load());
    invariant(!_client);

    // Reclaim the client.
    _client = Client::releaseCurrent();
    invariant(_client.get() == _clientPtr, kUnableToRecoverClient);

    if (_oldThreadName) {
        // Reset the last thread name because it was previously set in the OS.
        setThreadNameRef(std::move(_oldThreadName));
    } else {
        // Release the thread name for reuse.
        releaseThreadNameRef();
    }

    LOGV2_DEBUG(
        5127803, kDiagnosticLogLevel, "Released the Client", "client"_attr = _client->desc());
}

}  // namespace mongo
