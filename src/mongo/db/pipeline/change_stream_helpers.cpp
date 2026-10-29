// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_helpers.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/util/assert_util.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::change_stream {

bool isRouterOrNonShardedReplicaSet(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (expCtx->getInRouter()) {
        return true;
    }
    if (expCtx->getForPerShardCursor()) {
        // Sharded cluster mongod.
        return false;
    }

    if (const auto* shardingState = ShardingState::get(expCtx->getOperationContext());
        shardingState) {
        auto role = shardingState->pollClusterRole();
        const bool isReplSet = !role.has_value();
        return isReplSet;
    } else {
        // Sharding state is not initialized. This is the case in unit tests and also on standalone
        // mongods. But on standalone mongods we do not support change streams, so we will never get
        // here on standalone mongods.
        return false;
    }
}

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

bool shouldEmitCollectionUUIDForChangeEvent(const DocumentSourceChangeStreamSpec& spec) {
    return spec.getShowExpandedEvents();
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

}  // namespace mongo::change_stream
