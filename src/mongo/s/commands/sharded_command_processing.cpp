/**
 *    Copyright (C) 2013 mongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/commands/sharded_command_processing.h"

#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/shard_id.h"

namespace mongo {

void appendWriteConcernErrorToCmdResponse(const ShardId& shardId,
                                          const BSONElement& wcErrorElem,
                                          BSONObjBuilder& responseBuilder) {
    WriteConcernErrorDetail wcError;
    std::string errMsg;
    auto wcErrorObj = wcErrorElem.Obj();
    if (!wcError.parseBSON(wcErrorObj, &errMsg)) {
        wcError.setErrMessage("Failed to parse writeConcernError: " + wcErrorObj.toString() +
                              ", Received error: " + errMsg);
    }
    wcError.setErrMessage(wcError.getErrMessage() + " at " + shardId.toString());
    responseBuilder.append("writeConcernError", wcError.toBSON());
}
}  // namespace mongo
