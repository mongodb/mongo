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

#include "mongo/db/pipeline/change_stream_helpers.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/util/assert_util.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace change_stream {

ResumeTokenData resolveResumeTokenFromSpec(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const DocumentSourceChangeStreamSpec& spec) {

    if (spec.getStartAfter()) {
        return spec.getStartAfter()->getData();
    } else if (spec.getResumeAfter()) {
        return spec.getResumeAfter()->getData();
    } else if (spec.getStartAtOperationTime()) {
        return ResumeToken::makeHighWaterMarkToken(*spec.getStartAtOperationTime(),
                                                   expCtx->getChangeStreamTokenVersion())
            .getData();
    }
    tasserted(5666901,
              "Expected one of 'startAfter', 'resumeAfter' or 'startAtOperationTime' to be "
              "populated in $changeStream spec");
}

repl::MutableOplogEntry createEndOfTransactionOplogEntry(
    const LogicalSessionId& lsid,
    const TxnNumber& txnNumber,
    const std::vector<NamespaceString>& affectedNamespaces,
    Timestamp timestamp,
    Date_t wallClock) {
    static const BSONObj kNoopEndOfTransactionMsgObject =
        BSON("msg" << BSON("endOfTransaction" << 1));

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setNss(NamespaceString::kAdminCommandNamespace);
    oplogEntry.setObject(kNoopEndOfTransactionMsgObject);

    oplogEntry.setSessionId(lsid);
    oplogEntry.setTxnNumber(txnNumber);

    BSONObjBuilder o2;
    {
        BSONArrayBuilder namespaces{o2.subarrayStart("endOfTransaction")};
        for (const auto& nss : affectedNamespaces) {
            namespaces.append(
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        }
    }
    {
        BSONObjBuilder sessionId{o2.subobjStart(repl::OplogEntry::kSessionIdFieldName)};
        lsid.serialize(&sessionId);
    }
    o2.append(repl::OplogEntry::kTxnNumberFieldName, static_cast<long long>(txnNumber));
    oplogEntry.setObject2(o2.obj());

    oplogEntry.setTimestamp(timestamp);
    oplogEntry.setWallClockTime(wallClock);
    return oplogEntry;
}

}  // namespace change_stream
}  // namespace mongo
