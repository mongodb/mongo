// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/current_op_stage.h"

#include "mongo/db/commands/fsync.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source_current_op.h"

#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;
const std::string_view kOpIdFieldName = "opid"sv;
const std::string_view kClientFieldName = "client"sv;
const std::string_view kMongosClientFieldName = "client_s"sv;
const std::string_view kShardFieldName = "shard"sv;
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
            doc.addField(std::string_view("fsyncLock"), Value(true));
        }

        // For operations on a shard, we change the opid from the raw numeric form to
        // 'shardname:opid'. We also change the fieldname 'client' to 'client_s' to indicate
        // that the IP is that of the mongos which initiated this request.
        for (auto&& elt : op) {
            std::string_view fieldName = elt.fieldNameStringData();

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
