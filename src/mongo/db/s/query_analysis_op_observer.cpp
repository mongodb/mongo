/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/s/query_analysis_op_observer.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_mongos.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

namespace {

const auto docToDeleteDecoration = OperationContext::declareDecoration<BSONObj>();

}  // namespace

void QueryAnalysisOpObserver::onInserts(OperationContext* opCtx,
                                        const CollectionPtr& coll,
                                        std::vector<InsertStatement>::const_iterator begin,
                                        std::vector<InsertStatement>::const_iterator end,
                                        std::vector<bool> fromMigrate,
                                        bool defaultFromMigrate) {
    if (analyze_shard_key::supportsCoordinatingQueryAnalysis(opCtx)) {
        if (coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace) {
            for (auto it = begin; it != end; ++it) {
                const auto parsedDoc = QueryAnalyzerDocument::parse(
                    IDLParserContext("QueryAnalysisOpObserver::onInserts"), it->doc);
                opCtx->recoveryUnit()->onCommit([parsedDoc](OperationContext* opCtx,
                                                            boost::optional<Timestamp>) {
                    analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationInsert(
                        parsedDoc);
                });
            }
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
                   coll->ns() == MongosType::ConfigNS) {
            for (auto it = begin; it != end; ++it) {
                const auto parsedDoc = uassertStatusOK(MongosType::fromBSON(it->doc));
                opCtx->recoveryUnit()->onCommit(
                    [parsedDoc](OperationContext* opCtx, boost::optional<Timestamp>) {
                        analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onSamplerInsert(
                            parsedDoc);
                    });
            }
        }
    }
}

void QueryAnalysisOpObserver::onUpdate(OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    if (analyze_shard_key::supportsCoordinatingQueryAnalysis(opCtx)) {
        if (args.coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace) {
            const auto parsedDoc = QueryAnalyzerDocument::parse(
                IDLParserContext("QueryAnalysisOpObserver::onUpdate"), args.updateArgs->updatedDoc);
            opCtx->recoveryUnit()->onCommit(
                [parsedDoc](OperationContext* opCtx, boost::optional<Timestamp>) {
                    analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationUpdate(
                        parsedDoc);
                });
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
                   args.coll->ns() == MongosType::ConfigNS) {
            const auto parsedDoc =
                uassertStatusOK(MongosType::fromBSON(args.updateArgs->updatedDoc));
            opCtx->recoveryUnit()->onCommit([parsedDoc](OperationContext* opCtx,
                                                        boost::optional<Timestamp>) {
                analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onSamplerUpdate(parsedDoc);
            });
        }
    }

    if (analyze_shard_key::supportsPersistingSampledQueries(opCtx) && args.updateArgs->sampleId &&
        opCtx->writesAreReplicated()) {
        analyze_shard_key::QueryAnalysisWriter::get(opCtx)
            ->addDiff(*args.updateArgs->sampleId,
                      args.coll->ns(),
                      args.coll->uuid(),
                      args.updateArgs->preImageDoc,
                      args.updateArgs->updatedDoc)
            .getAsync([](auto) {});
    }
}

void QueryAnalysisOpObserver::aboutToDelete(OperationContext* opCtx,
                                            const CollectionPtr& coll,
                                            BSONObj const& doc) {
    if (analyze_shard_key::supportsCoordinatingQueryAnalysis(opCtx)) {
        if (coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace ||
            coll->ns() == MongosType::ConfigNS) {
            docToDeleteDecoration(opCtx) = doc;
        }
    }
}

void QueryAnalysisOpObserver::onDelete(OperationContext* opCtx,
                                       const CollectionPtr& coll,
                                       StmtId stmtId,
                                       const OplogDeleteEntryArgs& args) {
    if (analyze_shard_key::supportsCoordinatingQueryAnalysis(opCtx)) {
        if (coll->ns() == NamespaceString::kConfigQueryAnalyzersNamespace) {
            auto& doc = docToDeleteDecoration(opCtx);
            invariant(!doc.isEmpty());
            const auto parsedDoc = QueryAnalyzerDocument::parse(
                IDLParserContext("QueryAnalysisOpObserver::onDelete"), doc);
            opCtx->recoveryUnit()->onCommit(
                [parsedDoc](OperationContext* opCtx, boost::optional<Timestamp>) {
                    analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationDelete(
                        parsedDoc);
                });
        } else if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) &&
                   coll->ns() == MongosType::ConfigNS) {
            auto& doc = docToDeleteDecoration(opCtx);
            invariant(!doc.isEmpty());
            const auto parsedDoc = uassertStatusOK(MongosType::fromBSON(doc));
            opCtx->recoveryUnit()->onCommit([parsedDoc](OperationContext* opCtx,
                                                        boost::optional<Timestamp>) {
                analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onSamplerDelete(parsedDoc);
            });
        }
    }
}

}  // namespace analyze_shard_key
}  // namespace mongo
