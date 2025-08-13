/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/query_analysis_op_observer_configsvr.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/type_mongos.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/query_analysis_op_observer.h"
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
