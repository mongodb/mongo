/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/rpc/legacy_request_builder.h"

#include <tuple>
#include <utility>

#include "mongo/client/read_preference.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

namespace {
void mergeInDocumentSequences(const OpMsgRequest& request, BSONObjBuilder* body) {
    for (auto&& seq : request.sequences) {
        invariant(seq.name.find('.') == std::string::npos);  // Only support top-level for now.
        dassert(!body->asTempObj().hasField(seq.name));
        body->append(seq.name, seq.objs);
    }
}

/**
 * Given a command request, attempts to construct a legacy command
 * object and query flags bitfield augmented with the given metadata.
 */
BSONObj downconvertRequestBody(const OpMsgRequest& request, int* queryOptions) {
    *queryOptions = 0;

    if (auto readPref = request.body["$readPreference"]) {
        auto parsed = ReadPreferenceSetting::fromInnerBSON(readPref);
        if (parsed.isOK() && parsed.getValue().canRunOnSecondary()) {
            *queryOptions |= QueryOption_SlaveOk;
        }

        BSONObjBuilder outer;
        {
            BSONObjBuilder inner(outer.subobjStart("$query"));
            for (auto field : request.body) {
                const auto name = field.fieldNameStringData();
                if (name == "$readPreference" || name == "$db") {
                    // skip field.
                } else {
                    inner.append(field);
                }
            }
            mergeInDocumentSequences(request, &inner);
        }
        outer.append(readPref);
        return outer.obj();
    } else {
        BSONObjBuilder body(request.body.removeField("$db"));
        mergeInDocumentSequences(request, &body);
        return body.obj();
    }
}
}  // namespace

Message legacyRequestFromOpMsgRequest(const OpMsgRequest& request) {
    BufBuilder builder;
    builder.skip(mongo::MsgData::MsgDataHeaderSize);

    const auto cmdNS = NamespaceString(request.getDatabase(), "").getCommandNS().toString();

    int queryOptions;
    const auto downconvertedBody = downconvertRequestBody(request, &queryOptions);

    builder.appendNum(queryOptions);
    builder.appendStr(cmdNS);
    builder.appendNum(0);  // nToSkip
    builder.appendNum(1);  // nToReturn

    downconvertedBody.appendSelfToBufBuilder(builder);

    MsgData::View msg = builder.buf();
    msg.setLen(builder.len());
    msg.setOperation(dbQuery);
    return Message(builder.release());
}

}  // namespace rpc
}  // namespace mongo
