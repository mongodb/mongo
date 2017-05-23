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

#include "mongo/rpc/command_request_builder.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/commands.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

namespace {
// OP_COMMAND put some generic arguments in the metadata and some in the body.
bool fieldGoesInMetadata(StringData commandName, StringData field) {
    if (!Command::isGenericArgument(field))
        return false;  // All non-generic arguments go to the body.

    // For some reason this goes in the body only for a single command...
    if (field == "$replData")
        return commandName != "replSetUpdatePosition";

    // These generic arguments went in the body.
    return !(field == "maxTimeMS" || field == "readConcern" || field == "writeConcern" ||
             field == "shardVersion");
}
}  // namespace

Message opCommandRequestFromOpMsgRequest(const OpMsgRequest& request) {
    invariant(request.sequences.empty());  // Not supported yet.

    const auto commandName = request.getCommandName();

    BufBuilder builder;
    builder.skip(mongo::MsgData::MsgDataHeaderSize);  // Leave room for message header.
    builder.appendStr(request.getDatabase());
    builder.appendStr(commandName);

    // OP_COMMAND is only used when communicating with 3.4 nodes and they serialize their metadata
    // fields differently. In addition to field-level differences, some generic arguments are pulled
    // out to a metadata object, separate from the body. We do all down-conversion here so that the
    // rest of the code only has to deal with the current format.
    BSONObjBuilder metadataBuilder;  // Will be appended to the message after we finish the body.
    {
        BSONObjBuilder bodyBuilder(builder);
        for (auto elem : request.body) {
            const auto fieldName = elem.fieldNameStringData();
            if (fieldName == "$configServerState") {
                metadataBuilder.appendAs(elem, "configsvr");
            } else if (fieldName == "$readPreference") {
                BSONObjBuilder ssmBuilder(metadataBuilder.subobjStart("$ssm"));
                ssmBuilder.append(elem);
                ssmBuilder.append("$secondaryOk",
                                  uassertStatusOK(ReadPreferenceSetting::fromInnerBSON(elem))
                                      .canRunOnSecondary());
            } else if (fieldName == "$db") {
                // skip
            } else if (fieldGoesInMetadata(commandName, fieldName)) {
                metadataBuilder.append(elem);
            } else {
                bodyBuilder.append(elem);
            }
        }
    }
    metadataBuilder.obj().appendSelfToBufBuilder(builder);

    MsgData::View msg = builder.buf();
    msg.setLen(builder.len());
    msg.setOperation(dbCommand);
    return Message(builder.release());
}

}  // namespace rpc
}  // namespace mongo
