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

#include "mongo/db/exec/agg/current_op_stage.h"

#include "mongo/db/commands/fsync_locked.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_current_op.h"

namespace mongo {

namespace {
const StringData kOpIdFieldName = "opid"_sd;
const StringData kClientFieldName = "client"_sd;
const StringData kMongosClientFieldName = "client_s"_sd;
const StringData kShardFieldName = "shard"_sd;
}  // namespace

boost::intrusive_ptr<exec::agg::Stage> documentSourceCurrentOpToStageFn(
    const boost::intrusive_ptr<const DocumentSource>& documentSource) {
    auto currentOp = boost::dynamic_pointer_cast<const DocumentSourceCurrentOp>(documentSource);

    tassert(10565700, "expected 'DocumentSourceCurrentOp' type", currentOp);

    return make_intrusive<exec::agg::CurrentOpStage>(currentOp->getExpCtx(),
                                                     currentOp->_includeIdleConnections,
                                                     currentOp->_includeIdleSessions,
                                                     currentOp->_includeOpsFromAllUsers,
                                                     currentOp->_truncateOps,
                                                     currentOp->_idleCursors);
}

namespace exec {
namespace agg {

REGISTER_AGG_STAGE_MAPPING(currentOp,
                           DocumentSourceCurrentOp::id,
                           documentSourceCurrentOpToStageFn);

CurrentOpStage::CurrentOpStage(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                               const boost::optional<ConnMode>& includeIdleConnections,
                               const boost::optional<SessionMode>& includeIdleSessions,
                               const boost::optional<UserMode>& includeOpsFromAllUsers,
                               const boost::optional<TruncationMode>& truncateOps,
                               const boost::optional<CursorMode>& idleCursors)
    : Stage(kStageName, pExpCtx),
      _includeIdleConnections(includeIdleConnections),
      _includeIdleSessions(includeIdleSessions),
      _includeOpsFromAllUsers(includeOpsFromAllUsers),
      _truncateOps(truncateOps),
      _idleCursors(idleCursors) {}

GetNextResult CurrentOpStage::doGetNext() {
    if (_ops.empty()) {
        _ops = pExpCtx->getMongoProcessInterface()->getCurrentOps(
            pExpCtx,
            _includeIdleConnections.value_or(kDefaultConnMode),
            _includeIdleSessions.value_or(kDefaultSessionMode),
            _includeOpsFromAllUsers.value_or(kDefaultUserMode),
            _truncateOps.value_or(kDefaultTruncationMode),
            _idleCursors.value_or(kDefaultCursorMode));

        _opsIter = _ops.begin();

        if (pExpCtx->getFromRouter()) {
            _shardName =
                pExpCtx->getMongoProcessInterface()->getShardName(pExpCtx->getOperationContext());

            uassert(40465,
                    "Aggregation request specified 'fromRouter' but unable to retrieve shard name "
                    "for $currentOp pipeline stage.",
                    !_shardName.empty());
        }
    }

    if (_opsIter != _ops.end()) {
        if (!pExpCtx->getFromRouter()) {
            return Document(*_opsIter++);
        }

        // This $currentOp is running in a sharded context.
        invariant(!_shardName.empty());

        const BSONObj& op = *_opsIter++;
        MutableDocument doc;

        // Add the shard name to the output document.
        doc.addField(kShardFieldName, Value(_shardName));

        if (mongo::lockedForWriting()) {
            doc.addField(StringData("fsyncLock"), Value(true));
        }

        // For operations on a shard, we change the opid from the raw numeric form to
        // 'shardname:opid'. We also change the fieldname 'client' to 'client_s' to indicate
        // that the IP is that of the mongos which initiated this request.
        for (auto&& elt : op) {
            StringData fieldName = elt.fieldNameStringData();

            if (fieldName == kOpIdFieldName) {
                uassert(ErrorCodes::TypeMismatch,
                        str::stream() << "expected numeric opid for $currentOp response from '"
                                      << _shardName << "' but got: " << typeName(elt.type()),
                        elt.isNumber());

                std::string shardOpID = (str::stream() << _shardName << ":" << elt.numberInt());
                doc.addField(kOpIdFieldName, Value(shardOpID));
            } else if (fieldName == kClientFieldName) {
                doc.addField(kMongosClientFieldName, Value(elt.str()));
            } else {
                doc.addField(fieldName, Value(elt));
            }
        }

        return doc.freeze();
    }

    return GetNextResult::makeEOF();
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
