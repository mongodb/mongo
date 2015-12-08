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
Status readRequestMetadata(OperationContext* txn, const BSONObj& metadataObj);

/**
 * Writes metadata from an OperationContext to a metadata object.
 */
Status writeRequestMetadata(OperationContext* txn, BSONObjBuilder* metadataBob);

/**
 * A command object and a corresponding metadata object.
 */
using CommandAndMetadata = std::tuple<BSONObj, BSONObj>;

/**
 * A legacy command object and a corresponding query flags bitfield. The legacy command object
 * may contain metadata fields, so it cannot safely be passed to a command's run method.
 */
using LegacyCommandAndFlags = std::tuple<BSONObj, int>;

/**
 * Given a legacy command object and a query flags bitfield, attempts to parse and remove
 * the metadata from the command object and construct a corresponding metadata object.
 */
StatusWith<CommandAndMetadata> upconvertRequestMetadata(BSONObj legacyCmdObj, int queryFlags);

/**
 * Given a command object and a metadata object, attempts to construct a legacy command
 * object and query flags bitfield augmented with the given metadata.
 */
StatusWith<LegacyCommandAndFlags> downconvertRequestMetadata(BSONObj cmdObj, BSONObj metadata);

/**
 * A command reply and associated metadata object.
 */
using CommandReplyWithMetadata = std::tuple<BSONObj, BSONObj>;

/**
 * Given a legacy command reply, attempts to strip the metadata from the reply and construct
 * a metadata object.
 */
StatusWith<CommandReplyWithMetadata> upconvertReplyMetadata(const BSONObj& legacyReply);

/**
 * Given a command reply object and an associated metadata object,
 * attempts to construct a legacy command object.
 */
StatusWith<BSONObj> downconvertReplyMetadata(const BSONObj& commandReply,
                                             const BSONObj& replyMetadata);

/**
 * A function type for writing request metadata. The function takes a pointer to a
 * BSONObjBuilder used to construct the metadata object and the server address of the
 * target of the request and returns a Status indicating if the metadata was written successfully.
 */
using RequestMetadataWriter = stdx::function<Status(BSONObjBuilder*, StringData)>;

/**
 * A function type for reading reply metadata. The function takes a a reference to a
 * metadata object received in a command reply and the server address of the
 * host that executed the command and returns a Status indicating if the
 * metadata was read successfully.
 *
 * TODO: would it be a layering violation if this hook took an OperationContext* ?
 */
using ReplyMetadataReader = stdx::function<Status(const BSONObj&, StringData)>;

}  // namespace rpc
}  // namespace mongo
