/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <iosfwd>
#include <memory>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/util/net/message.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace rpc {
class ReplyInterface;
}  // namespace rpc

namespace executor {


/**
 * Type of object describing the response of previously sent RemoteCommandRequest.
 */
struct RemoteCommandResponse {
    RemoteCommandResponse() = default;

    RemoteCommandResponse(BSONObj dataObj, BSONObj metadataObj)
        : RemoteCommandResponse(dataObj, metadataObj, Milliseconds(0)) {}

    RemoteCommandResponse(BSONObj dataObj, BSONObj metadataObj, Milliseconds millis)
        : data(std::move(dataObj)), metadata(std::move(metadataObj)), elapsedMillis(millis) {
        // The buffer backing the default empty BSONObj has static duration so it is effectively
        // owned.
        invariant(data.isOwned() || data.objdata() == BSONObj().objdata());
        invariant(metadata.isOwned() || metadata.objdata() == BSONObj().objdata());
    }

    RemoteCommandResponse(Message messageArg,
                          BSONObj dataObj,
                          BSONObj metadataObj,
                          Milliseconds millis)
        : message(std::make_shared<const Message>(std::move(messageArg))),
          data(std::move(dataObj)),
          metadata(std::move(metadataObj)),
          elapsedMillis(millis) {
        if (!data.isOwned()) {
            data.shareOwnershipWith(message->sharedBuffer());
        }
        if (!metadata.isOwned()) {
            metadata.shareOwnershipWith(message->sharedBuffer());
        }
    }

    RemoteCommandResponse(const rpc::ReplyInterface& rpcReply, Milliseconds millis);

    std::string toString() const;

    bool operator==(const RemoteCommandResponse& rhs) const;
    bool operator!=(const RemoteCommandResponse& rhs) const;

    std::shared_ptr<const Message> message;  // May be null.
    BSONObj data;                            // Always owned. May point into message.
    BSONObj metadata;                        // Always owned. May point into message.
    Milliseconds elapsedMillis = {};
};

}  // namespace executor

std::ostream& operator<<(std::ostream& os, const executor::RemoteCommandResponse& request);

}  // namespace mongo
