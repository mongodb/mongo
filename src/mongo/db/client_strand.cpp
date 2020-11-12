/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/client_strand.h"

#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/thread_context.h"

namespace mongo {
namespace {
struct ClientStrandData {
    ClientStrand* strand = nullptr;
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

void ClientStrand::_setCurrent() noexcept {
    invariant(_isBound.load());
    invariant(_client);

    LOGV2_DEBUG(
        5127801, kDiagnosticLogLevel, "Setting the Client", "client"_attr = _client->desc());

    // Set the Client for this thread so calls to Client::getCurrent() works as expected.
    Client::setCurrent(std::move(_client));

    // Set up the thread name.
    _oldThreadName = ThreadName::set(ThreadContext::get(), _threadName);
    if (_oldThreadName) {
        LOGV2_DEBUG(5127802, kDiagnosticLogLevel, "Set thread name", "name"_attr = *_threadName);
    }
}

void ClientStrand::_releaseCurrent() noexcept {
    invariant(_isBound.load());
    invariant(!_client);

    // Reclaim the client.
    _client = Client::releaseCurrent();
    invariant(_client.get() == _clientPtr, kUnableToRecoverClient);

    if (_oldThreadName) {
        // Reset the last thread name because it was previously set in the OS.
        ThreadName::set(ThreadContext::get(), std::move(_oldThreadName));
    } else {
        // Release the thread name for reuse.
        ThreadName::release(ThreadContext::get());
    }

    LOGV2_DEBUG(
        5127803, kDiagnosticLogLevel, "Released the Client", "client"_attr = _client->desc());
}

}  // namespace mongo
