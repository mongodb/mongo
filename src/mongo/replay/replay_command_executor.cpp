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
#include "mongo/logv2/log.h"
#include "mongo/replay/replay_command.h"
#include "mongo/util/assert_util.h"

#include <chrono>
#include <string>
#include <thread>

namespace mongo {

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
    uassert(ErrorCodes::ReplayClientNotConnected, "MongoR is not connected", isConnected());
    _dbConnection->reset();
    _dbConnection.reset();
}

bool ReplayCommandExecutor::isConnected() const {
    return _dbConnection && _dbConnection->isStillConnected();
}

BSONObj ReplayCommandExecutor::runCommand(const ReplayCommand& command) const {
    uassert(ErrorCodes::ReplayClientNotConnected, "MongoR is not connected", isConnected());
    OpMsgRequest request = command.fetchMsgRequest();
    try {
        const auto reply = _dbConnection->runCommand(std::move(request));
        return reply->getCommandReply().getOwned();
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tassert(ErrorCodes::ReplayClientInternalError, lastError.reason(), false);
    }
    return {};
}


}  // namespace mongo
