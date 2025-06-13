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

#include "mongo/replay/replay_command.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/replay/replay_command_executor.h"
#include "mongo/rpc/factory.h"

#include <exception>

namespace mongo {

inline OpMsgRequest parse(BSONObj bsonCommand) {

    // The bsonobj stored inside ReplayCommand must have this format. We can only parse traffic
    // recording bson like packets.
    // rawop: { header: { messagelength: 339, requestid: 2, responseto: 0, opcode: 2004 },
    //          body: BinData(0, ...) }

    BSONElement bodyElem = bsonCommand["body"];
    int len = 0;
    const char* data = static_cast<const char*>(bodyElem.binDataClean(len));
    Message message;
    const auto layoutSize = sizeof(MsgData::Layout);
    message.setData(dbMsg, data + layoutSize, len - layoutSize);
    OpMsg::removeChecksum(&message);
    return rpc::opMsgRequestFromAnyProtocol(message);
}

bool ReplayCommand::toRequest(OpMsgRequest& request) const {
    try {
        request = parse(_bsonCommand);
    } catch (const DBException& e) {
        auto lastError = e.toStatus();
        tassert(ErrorCodes::InternalError, lastError.reason(), false);
        return false;
    } catch (const std::exception& e) {
        tassert(ErrorCodes::InternalError, e.what(), false);
        return false;
    }
    return true;
}

std::string ReplayCommand::toString() const {
    try {
        const auto request = parse(_bsonCommand);
        return request.body.toString();
    } catch (const DBException& e) {
        tassert(ErrorCodes::InternalError, e.what(), false);
    }
    return {};
}

}  // namespace mongo
