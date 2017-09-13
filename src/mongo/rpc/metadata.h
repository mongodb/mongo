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

#include <tuple>

#include "mongo/base/status_with.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/net/op_msg.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class OperationContext;
class StringData;

/**
 * Utilities for converting metadata between the legacy OP_QUERY format and the new
 * OP_COMMAND format.
 */
namespace rpc {

/**
 * Returns an empty metadata object.
 */
BSONObj makeEmptyMetadata();

/**
 * Reads metadata from a metadata object and sets it on this OperationContext.
 */
void readRequestMetadata(OperationContext* opCtx, const BSONObj& metadataObj);

/**
 * A legacy command object and a corresponding query flags bitfield. The legacy command object
 * may contain metadata fields, so it cannot safely be passed to a command's run method.
 */
using LegacyCommandAndFlags = std::tuple<BSONObj, int>;

/**
 * Upconverts a legacy command request into an OpMessageRequest.
 */
OpMsgRequest upconvertRequest(StringData db, BSONObj legacyCmdObj, int queryFlags);

/**
 * A function type for writing request metadata. The function takes a pointer to an optional
 * OperationContext so metadata associated with a Client can be appended, a pointer to a
 * BSONObjBuilder used to construct the metadata object and returns a Status indicating if the
 * metadata was written successfully.
 */
using RequestMetadataWriter =
    stdx::function<Status(OperationContext* opCtx, BSONObjBuilder* metadataBuilder)>;

/**
 * A function type for reading reply metadata. The function takes a a reference to a
 * metadata object received in a command reply and the server address of the
 * host that executed the command and returns a Status indicating if the
 * metadata was read successfully.
 *
 * TODO: would it be a layering violation if this hook took an OperationContext* ?
 */
using ReplyMetadataReader = stdx::function<Status(
    OperationContext* opCtx, const BSONObj& replyMetadata, StringData sourceHost)>;

}  // namespace rpc
}  // namespace mongo
