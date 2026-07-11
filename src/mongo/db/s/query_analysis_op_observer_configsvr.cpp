// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/query_analysis_op_observer_configsvr.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_mongos.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/query_analysis_op_observer.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

void QueryAnalysisOpObserverConfigSvr::onInserts(OperationContext* opCtx,
                                                 const CollectionPtr& coll,
                                                 std::vector<InsertStatement>::const_iterator begin,
                                                 std::vector<InsertStatement>::const_iterator end,
                                                 const std::vector<RecordId>& recordIds,
                                                 std::vector<bool> fromMigrate,
                                                 bool defaultFromMigrate,
                                                 OpStateAccumulator* opAccumulator) {
    const auto& ns = coll->ns();

    if (ns == NamespaceString::kConfigQueryAnalyzersNamespace) {
        insertInConfigQueryAnalyzersNamespaceImpl(opCtx, begin, end);
    } else if (ns == MongosType::ConfigNS) {
        for (auto it = begin; it != end; ++it) {
            const auto parsedDoc = uassertStatusOK(MongosType::fromBSON(it->doc));
            shard_role_details::getRecoveryUnit(opCtx)->onCommit(
                [this, parsedDoc](OperationContext* opCtx, boost::optional<Timestamp>) {
                    _onInserts(opCtx, parsedDoc);
                });
        }
    }
}

void QueryAnalysisOpObserverConfigSvr::onUpdate(OperationContext* opCtx,
                                                const OplogUpdateEntryArgs& args,
                                                OpStateAccumulator* opAccumulator) {
    const auto& ns = args.coll->ns();

    if (ns == NamespaceString::kConfigQueryAnalyzersNamespace) {
        updateToConfigQueryAnalyzersNamespaceImpl(opCtx, args);
    } else if (ns == MongosType::ConfigNS) {
        const auto parsedDoc = uassertStatusOK(MongosType::fromBSON(args.updateArgs->updatedDoc));
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [this, parsedDoc](OperationContext* opCtx, boost::optional<Timestamp>) {
                _onUpdate(opCtx, parsedDoc);
            });
    }
}

void QueryAnalysisOpObserverConfigSvr::onDelete(OperationContext* opCtx,
                                                const CollectionPtr& coll,
                                                StmtId stmtId,
                                                const BSONObj& doc,
                                                const DocumentKey& documentKey,
                                                const OplogDeleteEntryArgs& args,
                                                OpStateAccumulator* opAccumulator) {
    const auto& ns = coll->ns();

    if (ns == NamespaceString::kConfigQueryAnalyzersNamespace) {
        invariant(!doc.isEmpty());
        deleteFromConfigQueryAnalyzersNamespaceImpl(opCtx, args, doc);
    } else if (ns == MongosType::ConfigNS) {
        invariant(!doc.isEmpty());
        const auto parsedDoc = uassertStatusOK(MongosType::fromBSON(doc));
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [this, parsedDoc](OperationContext* opCtx, boost::optional<Timestamp>) {
                _onDelete(opCtx, parsedDoc);
            });
    }
}

void QueryAnalysisOpObserverConfigSvr::_onInserts(OperationContext* opCtx, const MongosType& doc) {
    try {
        _coordinatorFactory->getQueryAnalysisCoordinator(opCtx)->onSamplerInsert(doc);
    } catch (const DBException& ex) {
        LOGV2_ERROR(10690305,
                    "Failed to insert sampler",
                    "sampler"_attr = doc,
                    "error"_attr = ex.toString());
    }
}

void QueryAnalysisOpObserverConfigSvr::_onUpdate(OperationContext* opCtx, const MongosType& doc) {
    try {
        _coordinatorFactory->getQueryAnalysisCoordinator(opCtx)->onSamplerUpdate(doc);
    } catch (const DBException& ex) {
        LOGV2_ERROR(10690306,
                    "Failed to update sampler",
                    "sampler"_attr = doc,
                    "error"_attr = ex.toString());
    }
}

void QueryAnalysisOpObserverConfigSvr::_onDelete(OperationContext* opCtx, const MongosType& doc) {
    try {
        _coordinatorFactory->getQueryAnalysisCoordinator(opCtx)->onSamplerDelete(doc);
    } catch (const DBException& ex) {
        LOGV2_ERROR(10690307,
                    "Failed to delete sampler",
                    "sampler"_attr = doc,
                    "error"_attr = ex.toString());
    }
}
}  // namespace analyze_shard_key
}  // namespace mongo
