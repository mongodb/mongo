// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/replay/replay_command_executor.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_connection.h"
#include "mongo/client/dbclient_session.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/logv2/log.h"
#include "mongo/replay/replay_command.h"
#include "mongo/util/assert_util.h"

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace mongo {

void ReplayCommandExecutor::connect(std::string_view uri) {
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
    uassert(ErrorCodes::ReplayClientNotConnected, "MongoR is not connected", isConnected());
    _dbConnection->reset();
    _dbConnection.reset();
}

bool ReplayCommandExecutor::isConnected() const {
    return _dbConnection && _dbConnection->isStillConnected();
}

BSONObj ReplayCommandExecutor::runCommand(const ReplayCommand& command) const {
    uassert(ErrorCodes::ReplayClientNotConnected, "MongoR is not connected", isConnected());
    try {
        const auto reply = _dbConnection->runCommand(command.fetchMsgRequest());
        return reply->getCommandReply().getOwned();
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tassert(ErrorCodes::ReplayClientInternalError, lastError.reason(), false);
    }
}

}  // namespace mongo
