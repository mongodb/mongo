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

#include "document_source_query_settings.h"

#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_query_settings_gen.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/exec/async_results_merger_params_gen.h"
#include "mongo/s/query/exec/document_source_merge_cursors.h"

#include <algorithm>

namespace mongo {

using namespace query_settings;

REGISTER_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(querySettings,
                                           DocumentSourceQuerySettings::LiteParsed::parse,
                                           DocumentSourceQuerySettings::createFromBson,
                                           AllowedWithApiStrict::kNeverInVersion1,
                                           &feature_flags::gFeatureFlagQuerySettings);
ALLOCATE_DOCUMENT_SOURCE_ID(querySettings, DocumentSourceQuerySettings::id)

DocumentSourceQuerySettings::DocumentSourceQuerySettings(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, bool showDebugQueryShape)
    : DocumentSource(kStageName, expCtx),
      exec::agg::Stage(kStageName, expCtx),
      _showDebugQueryShape(showDebugQueryShape) {}

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

DocumentSource::GetNextResult createResult(OperationContext* opCtx,
                                           const boost::optional<TenantId>& tenantId,
                                           const QueryShapeConfiguration& configuration,
                                           bool includeDebugQueryShape) try {
    BSONObjBuilder bob;
    configuration.serialize(&bob);
    if (includeDebugQueryShape && configuration.getRepresentativeQuery()) {
        bob.append(DocumentSourceQuerySettings::kDebugQueryShapeFieldName,
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

DocumentSource::GetNextResult DocumentSourceQuerySettings::doGetNext() {
    switch (_state) {
        case State::kReadingFromQueryShapeRepresentativeQueriesColl: {
            DocumentSource::GetNextResult::ReturnStatus status;
            do {
                auto result = (*_queryShapeRepresentativeQueriesCursor)->getNext();
                status = result.getStatus();

                if (status == DocumentSource::GetNextResult::ReturnStatus::kAdvanced) {
                    auto representativeQuery = QueryShapeRepresentativeQuery::parse(
                        IDLParserContext{"QueryShapeRepresentativeQuery"},
                        result.getDocument().toBson());

                    // We ignore the "orphaned" representative queries that have no corresponding
                    // entry in the '_queryShapeConfigsMap'.
                    if (auto elt = _queryShapeConfigsMap->extract(representativeQuery.get_id())) {
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
            _iterator = _queryShapeConfigsMap->begin();
            [[fallthrough]];
        }

        case State::kReadingFromQueryShapeConfigsMap: {
            if (_iterator == _queryShapeConfigsMap->end()) {
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

boost::intrusive_ptr<DocumentSource>
DocumentSourceQuerySettings::createQueryShapeRepresentativeQueriesCursor(OperationContext* opCtx) {
    if (!feature_flags::gFeatureFlagPQSBackfill.isEnabled(
            VersionContext::getDecoration(opCtx),
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        return DocumentSourceQueue::create(getContext());
    }

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
        auto shardResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
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

boost::intrusive_ptr<DocumentSource> DocumentSourceQuerySettings::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(7746801,
            "$querySettings stage expects a document as argument",
            elem.type() == BSONType::Object);

    // Resolve whether to include the debug query shape or not.
    bool showDebugQueryShape = DocumentSourceQuerySettingsSpec::parse(
                                   IDLParserContext("$querySettings"), elem.embeddedObject())
                                   .getShowDebugQueryShape();
    return make_intrusive<DocumentSourceQuerySettings>(expCtx, showDebugQueryShape);
}

Value DocumentSourceQuerySettings::serialize(const SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << DOC("showDebugQueryShape" << Value(_showDebugQueryShape))));
}

void DocumentSourceQuerySettings::detachFromOperationContext() {
    (*_queryShapeRepresentativeQueriesCursor)->detachFromOperationContext();
}

void DocumentSourceQuerySettings::reattachToOperationContext(OperationContext* opCtx) {
    (*_queryShapeRepresentativeQueriesCursor)->reattachToOperationContext(opCtx);
}
}  // namespace mongo
