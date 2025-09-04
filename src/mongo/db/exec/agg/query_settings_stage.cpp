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

#include "mongo/db/exec/agg/query_settings_stage.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_query_settings.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceQuerySettingsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSourceQuerySettings) {
    auto* ptr = dynamic_cast<DocumentSourceQuerySettings*>(documentSourceQuerySettings.get());
    tassert(10885900, "expected 'DocumentSourceQuerySettings' type", ptr);
    return make_intrusive<exec::agg::QuerySettingsStage>(
        DocumentSourceQuerySettings::kStageName, ptr->getExpCtx(), ptr->getShowDebugQueryShape());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(querySettingsStage,
                           DocumentSourceQuerySettings::id,
                           documentSourceQuerySettingsToStageFn);

using namespace query_settings;

namespace {
BSONObj createDebugQueryShape(const BSONObj& representativeQuery,
                              OperationContext* opCtx,
                              const boost::optional<TenantId>& tenantId) {
    // Get the serialized query shape by creating the representative information for the given
    // representative query.
    const auto representativeInfo =
        query_settings::createRepresentativeInfo(opCtx, representativeQuery, tenantId);
    return representativeInfo.serializedQueryShape;
}

GetNextResult createResult(OperationContext* opCtx,
                           const boost::optional<TenantId>& tenantId,
                           const QueryShapeConfiguration& configuration,
                           bool includeDebugQueryShape) try {
    BSONObjBuilder bob;
    configuration.serialize(&bob);
    if (includeDebugQueryShape && configuration.getRepresentativeQuery()) {
        bob.append(QuerySettingsStage::kDebugQueryShapeFieldName,
                   createDebugQueryShape(*configuration.getRepresentativeQuery(), opCtx, tenantId));
    }
    return Document{bob.obj()};
} catch (const ExceptionFor<ErrorCodes::BSONObjectTooLarge>&) {
    uasserted(ErrorCodes::BSONObjectTooLarge,
              str::stream() << "query settings object exceeds " << BSONObjMaxInternalSize
                            << " bytes"
                            << (includeDebugQueryShape
                                    ? "; consider not setting 'showDebugQueryShape' to true"
                                    : ""));
}
}  // namespace

QuerySettingsStage::QuerySettingsStage(StringData stageName,
                                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       bool showDebugQueryShape)
    : Stage(stageName, pExpCtx), _showDebugQueryShape(showDebugQueryShape) {
    _queryShapeRepresentativeQueriesCursor = exec::agg::buildStage(
        createQueryShapeRepresentativeQueriesCursor(getContext()->getOperationContext()));

    // Get all query shape configurations owned by 'tenantId'.
    auto tenantId = getContext()->getNamespaceString().tenantId();
    auto* opCtx = getContext()->getOperationContext();
    auto configs = query_settings::QuerySettingsService::get(opCtx)
                       .getAllQueryShapeConfigurations(tenantId)
                       .queryShapeConfigurations;
    for (auto&& config : configs) {
        _queryShapeConfigsMap.emplace(config.getQueryShapeHash(), std::move(config));
    }
}

GetNextResult QuerySettingsStage::doGetNext() {
    switch (_state) {
        case State::kReadingFromQueryShapeRepresentativeQueriesColl: {
            DocumentSource::GetNextResult::ReturnStatus status;
            do {
                auto result = _queryShapeRepresentativeQueriesCursor->getNext();
                status = result.getStatus();

                if (status == DocumentSource::GetNextResult::ReturnStatus::kAdvanced) {
                    auto representativeQuery = QueryShapeRepresentativeQuery::parse(
                        result.getDocument().toBson(),
                        IDLParserContext{"QueryShapeRepresentativeQuery"});

                    // We ignore the "orphaned" representative queries that have no corresponding
                    // entry in the '_queryShapeConfigsMap'.
                    if (auto elt = _queryShapeConfigsMap.extract(representativeQuery.get_id())) {
                        auto queryShapeConfig = std::move(elt.mapped());
                        queryShapeConfig.setRepresentativeQuery(
                            representativeQuery.getRepresentativeQuery());
                        return createResult(getContext()->getOperationContext(),
                                            getContext()->getNamespaceString().tenantId(),
                                            queryShapeConfig,
                                            _showDebugQueryShape);
                    }
                }
            } while (status != DocumentSource::GetNextResult::ReturnStatus::kEOF);

            _state = State::kReadingFromQueryShapeConfigsMap;
            _iterator = _queryShapeConfigsMap.begin();
            [[fallthrough]];
        }

        case State::kReadingFromQueryShapeConfigsMap: {
            if (_iterator == _queryShapeConfigsMap.end()) {
                return DocumentSource::GetNextResult::makeEOF();
            }

            const auto& queryShapeConfig = _iterator->second;
            ++_iterator;

            return createResult(getContext()->getOperationContext(),
                                getContext()->getNamespaceString().tenantId(),
                                queryShapeConfig,
                                _showDebugQueryShape);
        }
    }

    MONGO_UNREACHABLE_TASSERT(10397700);
}

void QuerySettingsStage::doDispose() {
    // Because $querySettings stage owns a separate exec::agg::Stage to provide data from the
    // dedicated 'queryShapeRepresentativeQueries' collection we need to be sure we dispose it
    // appropriately.
    if (_queryShapeRepresentativeQueriesCursor) {
        _queryShapeRepresentativeQueriesCursor->dispose();
    }
}

boost::intrusive_ptr<DocumentSource>
QuerySettingsStage::createQueryShapeRepresentativeQueriesCursor(OperationContext* opCtx) {
    auto nss = NamespaceString::kQueryShapeRepresentativeQueriesNamespace;
    auto* grid = Grid::get(opCtx);
    if (getContext()->getInRouter() || grid->isShardingInitialized()) {
        // In case we are running in sharded cluster, open a cursor over the representativeQueries
        // collection on the configsvr and wrap it into DocumentSourceMergeCursors for easy
        // consumption.
        FindCommandRequest findRepresentativeQueriesCmd{nss};
        auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        findRepresentativeQueriesCmd.setReadConcern(readConcernArgs);

        auto configShard = grid->shardRegistry()->getConfigShard();
        auto shardResponse = uassertStatusOK(
            configShard->runCommand(opCtx,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    nss.dbName(),
                                    findRepresentativeQueriesCmd.toBSON(),
                                    Shard::RetryPolicy::kIdempotent));
        CursorResponse cursorResponse =
            uassertStatusOK(CursorResponse::parseFromBSON(shardResponse.response));

        std::vector<RemoteCursor> remoteCursors;
        remoteCursors.emplace_back(configShard->getId().toString(),
                                   configShard->getConnString().getServers()[0],
                                   std::move(cursorResponse));
        AsyncResultsMergerParams params(std::move(remoteCursors),
                                        NamespaceString::kQueryShapeRepresentativeQueriesNamespace);
        // TODO SERVER-107922: Construct the execution stage directly.
        return DocumentSourceMergeCursors::create(getContext(), std::move(params));
    } else {
        // In case we are running in a replica set, create a collectionScan executor and attach it
        // to the DocumentSourceCursor stage for easy consumption.
        MultipleCollectionAccessor collAccessor(acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kRead),
            MODE_IS));
        if (!collAccessor.getMainCollectionAcquisition().exists()) {
            return DocumentSourceQueue::create(getContext());
        }

        auto collScanExecutor = InternalPlanner::collectionScan(
            {.opCtx = opCtx,
             .collection = collAccessor.getMainCollectionPtrOrAcquisition(),
             .yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
             .plannerOptions = QueryPlannerParams::Options::RETURN_OWNED_DATA});

        // DocumentSourceCursor::getNext() restores the executor state prior reading from it. For it
        // to work properly we need to save the state first.
        collScanExecutor->saveState();

        auto stasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
        auto catalogResourceHandle = make_intrusive<DSCursorCatalogResourceHandle>(stasher);
        // TODO SERVER-107922: Construct the execution stage directly.
        auto cursor = DocumentSourceCursor::create(collAccessor,
                                                   std::move(collScanExecutor),
                                                   std::move(catalogResourceHandle),
                                                   getContext(),
                                                   DocumentSourceCursor::CursorType::kRegular);

        // 'catalogResourceHandle' and 'stasher' are responsible for acquiring and relinquishing the
        // locks. In order for it to work we need to stash the resources first.
        stashTransactionResourcesFromOperationContext(opCtx, stasher.get());

        return cursor;
    }
}

void QuerySettingsStage::detachFromOperationContext() {
    _queryShapeRepresentativeQueriesCursor->detachFromOperationContext();
}

void QuerySettingsStage::reattachToOperationContext(OperationContext* opCtx) {
    _queryShapeRepresentativeQueriesCursor->reattachToOperationContext(opCtx);
}
}  // namespace exec::agg
}  // namespace mongo
