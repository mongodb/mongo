/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/replay/replay_command_executor.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_session.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/replay/replay_command.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/periodic_runner_factory.h"
#include "mongo/util/version.h"

#include <chrono>
#include <string>

namespace mongo {

bool ReplayCommandExecutor::init() {
    // Run global init stuff. Very important for working.
    uassertStatusOK(mongo::runGlobalInitializers({}));
    // Setup client related stuff. Same as mongo shell.
    setup();
    return true;
}

void ReplayCommandExecutor::connect(StringData uri) {
    // Connect to mongo d/s instance and keep instance of the connection alive as long as this
    // object is alive.
    auto mongoURI = uassertStatusOK(MongoURI::parse(uri));
    std::string errmsg;
    auto connection = mongoURI.connect("MongoR", errmsg);
    uassert(ErrorCodes::InternalError, errmsg, connection);
    _dbConnection.reset(connection);
}

void ReplayCommandExecutor::reset() {
    // When a session is closed, the connection itself can be reused. This method reset the
    // connection.
    uassert(ErrorCodes::InternalError, "MongoR is not connected", isConnected());
    _dbConnection->reset();
    _dbConnection.reset(nullptr);
}

bool ReplayCommandExecutor::isConnected() const {
    return _dbConnection && _dbConnection->isStillConnected();
}


BSONObj ReplayCommandExecutor::runCommand(const ReplayCommand& command) const {
    OpMsgRequest request;
    uassert(ErrorCodes::ReplayClientNotConnected, "MongoR is not connected", isConnected());
    uassert(ErrorCodes::ReplayClientFailedToProcessBSON,
            "Failed to process bson command",
            command.toRequest(request));
    try {
        const auto reply = _dbConnection->runCommand(std::move(request));
        return reply->getCommandReply().getOwned();
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tassert(ErrorCodes::ReplayClientInternalError, lastError.reason(), false);
    }
    return {};
}

void ReplayCommandExecutor::setup() const {
    // If we call this using a mongo shell util, we already have a service context! However, if
    // we run the client as a standalone binary, we need to do this work ourselves.
    if (!hasGlobalServiceContext()) {
        auto serviceContext = ServiceContext::make();
        setupTransportLayer(*serviceContext);
        setupWireProtocol(*serviceContext);
        setGlobalServiceContext(std::move(serviceContext));
    }
}

void ReplayCommandExecutor::setupTransportLayer(mongo::ServiceContext& serviceContext) const {
    // Create transport layer.
    transport::AsioTransportLayer::Options opts;
    opts.mode = transport::AsioTransportLayer::Options::kEgress;
    auto sm = std::make_unique<transport::AsioSessionManager>(&serviceContext);
    auto tl = std::make_unique<mongo::transport::AsioTransportLayer>(opts, std::move(sm));
    serviceContext.setTransportLayerManager(
        std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
    auto tlPtr = serviceContext.getTransportLayerManager();
    uassertStatusOK(tlPtr->setup());
    uassertStatusOK(tlPtr->start());
}

void ReplayCommandExecutor::setupWireProtocol(mongo::ServiceContext& serviceContext) const {
    // Setup wire protocol specs
    WireSpec::Specification spec;
    spec.incomingInternalClient.minWireVersion = RELEASE_2_4_AND_BEFORE;
    spec.incomingInternalClient.maxWireVersion = LATEST_WIRE_VERSION;
    spec.outgoing.minWireVersion = SUPPORTS_OP_MSG;
    spec.outgoing.maxWireVersion = LATEST_WIRE_VERSION;
    spec.isInternalClient = true;
    WireSpec::getWireSpec(&serviceContext).initialize(std::move(spec));
}

}  // namespace mongo
