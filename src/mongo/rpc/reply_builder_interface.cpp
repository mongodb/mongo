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

#include "mongo/platform/basic.h"

#include "mongo/rpc/reply_builder_interface.h"

#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/net/message.h"


namespace mongo {
namespace rpc {

namespace {
const char kOKField[] = "ok";
const char kCodeField[] = "code";
const char kErrorField[] = "errmsg";

// similar to appendCommandStatus (duplicating logic here to avoid cyclic library
// dependency)
BSONObj augmentReplyWithStatus(const Status& status, const BSONObj& reply) {
    auto okField = reply.getField(kOKField);
    if (!okField.eoo() && okField.trueValue()) {
        return reply;
    }

    BSONObjBuilder bob;
    bob.appendElements(reply);
    if (okField.eoo()) {
        bob.append(kOKField, status.isOK() ? 1.0 : 0.0);
    }
    if (status.isOK()) {
        return bob.obj();
    }

    if (!reply.hasField(kErrorField)) {
        bob.append(kErrorField, status.reason());
    }

    if (!reply.hasField(kCodeField)) {
        bob.append(kCodeField, status.code());
    }

    return bob.obj();
}
}

ReplyBuilderInterface& ReplyBuilderInterface::setCommandReply(StatusWith<BSONObj> commandReply) {
    auto reply = commandReply.isOK() ? std::move(commandReply.getValue()) : BSONObj();
    return setRawCommandReply(augmentReplyWithStatus(commandReply.getStatus(), reply));
}

ReplyBuilderInterface& ReplyBuilderInterface::setCommandReply(Status nonOKStatus,
                                                              const BSONObj& extraErrorInfo) {
    invariant(!nonOKStatus.isOK());
    return setRawCommandReply(augmentReplyWithStatus(nonOKStatus, extraErrorInfo));
}

}  // namespace rpc
}  // namespace mongo
