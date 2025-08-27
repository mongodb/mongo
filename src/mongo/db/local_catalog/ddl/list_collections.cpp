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


#include "mongo/base/checked_cast.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/global_catalog/ddl/shuffle_list_command_results.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_catalog_helper.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/ddl/list_collections_filter.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/index_catalog.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/views/view.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <iosfwd>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace {

// Failpoint which causes to hang "listCollections" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeListCollections);

/**
 * Determines if 'matcher' is an exact match on the "name" field. If so, returns a vector of all
 * the collection names it is matching against. Returns {} if there is no obvious exact match on
 * name.
 *
 * Note the collection names returned are not guaranteed to exist, nor are they guaranteed to
 * match 'matcher'.
 */
boost::optional<vector<StringData>> _getExactNameMatches(const MatchExpression* matcher) {
    if (!matcher) {
        return {};
    }

    MatchExpression::MatchType matchType(matcher->matchType());
    if (matchType == MatchExpression::EQ) {
        auto eqMatch = checked_cast<const EqualityMatchExpression*>(matcher);
        if (eqMatch->path() == "name") {
            StringData name(eqMatch->getData().valueStringDataSafe());
            if (name.size()) {
                return {vector<StringData>{name}};
            } else {
                return vector<StringData>();
            }
        }
    } else if (matchType == MatchExpression::MATCH_IN) {
        auto matchIn = checked_cast<const InMatchExpression*>(matcher);
        if (matchIn->path() == "name" && matchIn->getRegexes().empty()) {
            vector<StringData> exactMatches;
            for (auto&& elem : matchIn->getEqualities()) {
                StringData name(elem.valueStringDataSafe());
                if (name.size()) {
                    exactMatches.push_back(elem.valueStringData());
                }
            }
            return {std::move(exactMatches)};
        }
    }
    return {};
}

/**
 * Uses 'matcher' to determine if the collection's information should be added to 'root'. If so,
 * allocates a WorkingSetMember containing information about 'collection', and adds it to
 * 'results'.
 *
 * Does not add any information about non-existent collections.
 */
void _addWorkingSetMember(OperationContext* opCtx,
                          const BSONObj& maybe,
                          const MatchExpression* matcher,
                          WorkingSet* ws,
                          std::vector<WorkingSetID>& results) {
    if (matcher && !exec::matcher::matchesBSON(matcher, maybe)) {
        return;
    }

    WorkingSetID id = ws->allocate();
    WorkingSetMember* member = ws->get(id);
    member->keyData.clear();
    member->recordId = RecordId();
    member->resetDocument(SnapshotId(), maybe);
    member->transitionToOwnedObj();
    results.emplace_back(std::move(id));
}

BSONObj buildViewBson(const ViewDefinition& view, bool nameOnly) {
    invariant(!view.timeseries());

    BSONObjBuilder b;
    b.append("name", view.name().coll());
    b.append("type", "view");

    if (nameOnly) {
        return b.obj();
    }

    BSONObjBuilder optionsBuilder(b.subobjStart("options"));
    optionsBuilder.append("viewOn", view.viewOn().coll());
    optionsBuilder.append("pipeline", view.pipeline());
    if (view.defaultCollator()) {
        optionsBuilder.append("collation", view.defaultCollator()->getSpec().toBSON());
    }
    optionsBuilder.doneFast();

    BSONObj info = BSON("readOnly" << true);
    b.append("info", info);
    return b.obj();
}

BSONObj buildTimeseriesBson(OperationContext* opCtx, const Collection* collection, bool nameOnly) {
    invariant(collection);

    BSONObjBuilder builder;
    builder.append("name", collection->ns().getTimeseriesViewNamespace().coll());
    builder.append("type", "timeseries");

    if (nameOnly) {
        return builder.obj();
    }

    builder.append("options",
                   collection->getCollectionOptions().toBSON(
                       false /* includeUUID */, timeseries::kAllowedCollectionCreationOptions));
    builder.append("info", BSON("readOnly" << opCtx->readOnly()));

    return builder.obj();
}

BSONObj buildTimeseriesBson(OperationContext* opCtx, StringData collName, bool nameOnly) {
    BSONObjBuilder builder;
    builder.append("name", collName);
    builder.append("type", "timeseries");

    if (nameOnly) {
        return builder.obj();
    }

    builder.append("options", BSONObj{});
    builder.append("info", BSON("readOnly" << opCtx->readOnly()));

    return builder.obj();
}

/**
 * Return an object describing the collection. Takes a collection lock if nameOnly is false.
 */
BSONObj buildCollectionBson(OperationContext* opCtx,
                            const Collection* collection,
                            bool nameOnly,
                            bool isRawData) {
    if (!collection) {
        return {};
    }
    const auto& nss = collection->ns();

    BSONObjBuilder b;
    b.append("name", nss.coll());

    const auto showAsTimeseries = collection->isTimeseriesCollection() &&
        collection->isNewTimeseriesWithoutView() && !isRawData;
    const auto collectionType = [&] {
        if (showAsTimeseries) {
            return "timeseries";
        } else {
            return "collection";
        }
    }();

    b.append("type", collectionType);

    if (nameOnly) {
        return b.obj();
    }

    const auto& options = collection->getCollectionOptions();
    const auto& includeOptionsFields = [&]() -> auto& {
        if (showAsTimeseries) {
            return timeseries::kAllowedCollectionCreationOptions;
        } else {
            static const StringDataSet INCLUDE_ALL_OPTIONS = {};
            return INCLUDE_ALL_OPTIONS;
        }
    }();

    // While the UUID is stored as a collection option, from the user's perspective it is an
    // unsettable read-only property, so put it in the 'info' section. Pass 'false' to toBSON so
    // it doesn't include 'uuid' here.
    b.append("options", options.toBSON(false /* includeUUID */, includeOptionsFields));

    BSONObjBuilder infoBuilder;
    infoBuilder.append("readOnly", opCtx->readOnly());
    if (options.uuid) {
        infoBuilder.appendElements(options.uuid->toBSON());
    }
    if (const auto configDebugDump =
            catalog::getConfigDebugDump(VersionContext::getDecoration(opCtx), nss);
        configDebugDump.has_value()) {
        infoBuilder.append("configDebugDump", *configDebugDump);
    }
    b.append("info", infoBuilder.obj());

    auto idIndex = collection->getIndexCatalog()->findIdIndex(opCtx);
    if (idIndex) {
        b.append("idIndex", idIndex->infoObj());
    }

    return b.obj();
}

ListCollectionsReply createListCollectionsCursorReply(
    CursorId cursorId,
    const NamespaceString& cursorNss,
    const SerializationContext& respSerializationContext,
    std::vector<mongo::ListCollectionsReplyItem>&& firstBatch) {
    return ListCollectionsReply(
        ListCollectionsReplyCursor(
            cursorId, cursorNss, std::move(firstBatch), respSerializationContext),
        respSerializationContext);
}

class CmdListCollections : public ListCollectionsCmdVersion1Gen<CmdListCollections> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kOptIn;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const final {
        return true;
    }

    std::string help() const final {
        return "list collections for this db";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        using InvocationBaseGen::InvocationBaseGen;
        bool supportsWriteConcern() const final {
            return false;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        bool supportsRawData() const final {
            return true;
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            AuthorizationSession* authzSession = AuthorizationSession::get(opCtx->getClient());
            uassertStatusOK(authzSession->checkAuthorizedToListCollections(request()));
        }

        NamespaceString ns() const final {
            return NamespaceString(request().getDbName());
        }

        Reply typedRun(OperationContext* opCtx) final {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            unique_ptr<MatchExpression> matcher;
            const auto as = AuthorizationSession::get(opCtx->getClient());

            const auto listCollRequest = request();
            const auto dbName = listCollRequest.getDbName();
            const bool nameOnly = listCollRequest.getNameOnly();
            const bool authorizedCollections = listCollRequest.getAuthorizedCollections();
            const bool isRawData = isRawDataOperation(opCtx);

            // We need to copy the serialization context from the request to the reply object
            const auto respSerializationContext =
                SerializationContext::stateCommandReply(listCollRequest.getSerializationContext());

            // The collator is null because collection objects are compared using binary
            // comparison.
            auto expCtx = ExpressionContextBuilder{}.opCtx(opCtx).ns(ns()).build();

            if (listCollRequest.getFilter()) {
                matcher = uassertStatusOK(
                    MatchExpressionParser::parse(*listCollRequest.getFilter(), expCtx));
            }

            const NamespaceString cursorNss = NamespaceString::makeListCollectionsNSS(dbName);
            std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
            std::vector<mongo::ListCollectionsReplyItem> firstBatch;
            {
                // Acquire only the global lock and set up a consistent in-memory catalog and
                // storage snapshot.
                AutoGetDbForReadMaybeLockFree lockFreeReadBlock(opCtx, dbName);
                auto catalog = CollectionCatalog::get(opCtx);

                CurOpFailpointHelpers::waitWhileFailPointEnabled(
                    &hangBeforeListCollections,
                    opCtx,
                    "hangBeforeListCollections",
                    []() {},
                    cursorNss);

                auto ws = std::make_unique<WorkingSet>();
                auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());
                std::vector<WorkingSetID> results;
                tassert(9089302,
                        "point in time catalog lookup for a collection list is not supported",
                        RecoveryUnit::ReadSource::kNoTimestamp ==
                            shard_role_details::getRecoveryUnit(opCtx)->getTimestampReadSource());

                if (DatabaseHolder::get(opCtx)->dbExists(opCtx, dbName)) {
                    if (auto collNames = _getExactNameMatches(matcher.get())) {
                        for (auto&& collName : *collNames) {
                            auto nss = NamespaceStringUtil::deserialize(dbName, collName);

                            // Only validate on a per-collection basis if the user requested
                            // a list of authorized collections
                            if (authorizedCollections &&
                                (!as->isAuthorizedForAnyActionOnResource(
                                    ResourcePattern::forExactNamespace(nss)))) {
                                continue;
                            }

                            auto collBson = [&] {
                                auto collection =
                                    catalog->establishConsistentCollection(opCtx, nss, boost::none);
                                if (collection) {
                                    return buildCollectionBson(
                                        opCtx, collection.get(), nameOnly, isRawData);
                                }

                                std::shared_ptr<const ViewDefinition> view =
                                    catalog->lookupView(opCtx, nss);
                                // TODO SERVER-101594: remove this once 9.0 becomes last LTS
                                // legacy timeseries view won't exist anymore.
                                if (view && view->timeseries()) {
                                    if (auto bucketsCollection =
                                            catalog->establishConsistentCollection(
                                                opCtx, view->viewOn(), boost::none)) {
                                        return buildTimeseriesBson(
                                            opCtx, bucketsCollection.get(), nameOnly);
                                    } else {
                                        // The buckets collection does not exist, so the
                                        // time-series view will be appended when we iterate
                                        // through the view catalog below.
                                    }
                                }

                                return BSONObj{};
                            }();

                            if (!collBson.isEmpty()) {
                                _addWorkingSetMember(
                                    opCtx, collBson, matcher.get(), ws.get(), results);
                            }
                        }
                    } else {
                        auto perCollectionWork = [&](const Collection* collection) {
                            // TODO SERVER-101594: remove this once 9.0 becomes last LTS
                            // buckets collection ('system.buckets') won't exists anymore.
                            if (collection->isTimeseriesCollection() &&
                                !collection->isNewTimeseriesWithoutView()) {
                                auto viewNss = collection->ns().getTimeseriesViewNamespace();
                                auto view =
                                    catalog->lookupViewWithoutValidatingDurable(opCtx, viewNss);
                                if (view && view->timeseries() &&
                                    (!authorizedCollections ||
                                     as->isAuthorizedForAnyActionOnResource(
                                         ResourcePattern::forExactNamespace(viewNss)))) {
                                    // The time-series view for this buckets namespace exists,
                                    // so add it here while we have the collection options.
                                    _addWorkingSetMember(
                                        opCtx,
                                        buildTimeseriesBson(opCtx, collection, nameOnly),
                                        matcher.get(),
                                        ws.get(),
                                        results);
                                }
                            }

                            if (authorizedCollections &&
                                (!as->isAuthorizedForAnyActionOnResource(
                                    ResourcePattern::forExactNamespace(collection->ns())))) {
                                return true;
                            }

                            BSONObj collBson =
                                buildCollectionBson(opCtx, collection, nameOnly, isRawData);
                            if (!collBson.isEmpty()) {
                                _addWorkingSetMember(
                                    opCtx, collBson, matcher.get(), ws.get(), results);
                            }

                            return true;
                        };

                        auto collections = catalog->establishConsistentCollections(opCtx, dbName);
                        for (const auto& collection : collections) {
                            perCollectionWork(collection.get());
                        }
                    }

                    // Skipping views is only necessary for internal cloning operations.
                    bool skipViews = listCollRequest.getFilter() &&
                        SimpleBSONObjComparator::kInstance.evaluate(
                            *listCollRequest.getFilter() ==
                            ListCollectionsFilter::makeTypeCollectionFilter());

                    if (!skipViews) {
                        catalog->iterateViews(opCtx, dbName, [&](const ViewDefinition& view) {
                            if (authorizedCollections &&
                                !as->isAuthorizedForAnyActionOnResource(
                                    ResourcePattern::forExactNamespace(view.name()))) {
                                return true;
                            }

                            // TODO SERVER-101594: remove once 9.0 becomes last LTS
                            // legacy timeseries view won't exist anymore.
                            if (view.timeseries()) {
                                if (!catalog->lookupCollectionByNamespace(opCtx, view.viewOn())) {
                                    // There is no buckets collection backing this time-series
                                    // view, which means that it was not already added along
                                    // with the buckets collection above.
                                    _addWorkingSetMember(
                                        opCtx,
                                        buildTimeseriesBson(opCtx, view.name().coll(), nameOnly),
                                        matcher.get(),
                                        ws.get(),
                                        results);
                                }
                                return true;
                            }

                            BSONObj viewBson = buildViewBson(view, nameOnly);
                            if (!viewBson.isEmpty()) {
                                _addWorkingSetMember(
                                    opCtx, viewBson, matcher.get(), ws.get(), results);
                            }
                            return true;
                        });
                    }
                }

                shuffleListCommandResults.execute([&](const auto&) {
                    std::random_device rd;
                    std::mt19937 g(rd());
                    std::shuffle(results.begin(), results.end(), g);
                });

                for (const auto& id : results) {
                    root->pushBack(id);
                }

                exec = uassertStatusOK(
                    plan_executor_factory::make(expCtx,
                                                std::move(ws),
                                                std::move(root),
                                                &CollectionPtr::null,
                                                PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                false, /* whether owned BSON must be returned */
                                                cursorNss));

                long long batchSize = std::numeric_limits<long long>::max();
                if (listCollRequest.getCursor() && listCollRequest.getCursor()->getBatchSize()) {
                    batchSize = *listCollRequest.getCursor()->getBatchSize();
                }

                FindCommon::BSONArrayResponseSizeTracker responseSizeTracker;
                for (long long objCount = 0; objCount < batchSize; objCount++) {
                    BSONObj nextDoc;
                    PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                    if (state == PlanExecutor::IS_EOF) {
                        break;
                    }
                    invariant(state == PlanExecutor::ADVANCED);

                    // If we can't fit this result inside the current batch, then we stash it
                    // for later.
                    if (!responseSizeTracker.haveSpaceForNext(nextDoc)) {
                        exec->stashResult(nextDoc);
                        break;
                    }

                    try {
                        firstBatch.push_back(ListCollectionsReplyItem::parse(
                            nextDoc,
                            IDLParserContext("ListCollectionsReplyItem",
                                             auth::ValidatedTenancyScope::get(opCtx),
                                             cursorNss.tenantId(),
                                             respSerializationContext)));
                    } catch (const DBException& exc) {
                        LOGV2_ERROR(
                            5254300,
                            "Could not parse catalog entry while replying to listCollections",
                            "entry"_attr = nextDoc,
                            "error"_attr = exc);
                        fassertFailed(5254301);
                    }
                    responseSizeTracker.add(nextDoc);
                }
                if (exec->isEOF()) {
                    return createListCollectionsCursorReply(0 /* cursorId */,
                                                            cursorNss,
                                                            respSerializationContext,
                                                            std::move(firstBatch));
                }
                exec->saveState();
                exec->detachFromOperationContext();
            }  // Drop db lock. Global cursor registration must be done without holding any
               // locks.

            auto cmdObj = listCollRequest.toBSON();
            auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
                opCtx,
                {std::move(exec),
                 cursorNss,
                 AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
                 APIParameters::get(opCtx),
                 opCtx->getWriteConcern(),
                 repl::ReadConcernArgs::get(opCtx),
                 ReadPreferenceSetting::get(opCtx),
                 cmdObj,
                 uassertStatusOK(AuthorizationSession::get(opCtx->getClient())
                                     ->checkAuthorizedToListCollections(listCollRequest))});
            pinnedCursor->incNBatches();
            pinnedCursor->incNReturnedSoFar(firstBatch.size());

            return createListCollectionsCursorReply(pinnedCursor.getCursor()->cursorid(),
                                                    cursorNss,
                                                    respSerializationContext,
                                                    std::move(firstBatch));
        }
    };
};
MONGO_REGISTER_COMMAND(CmdListCollections).forShard();

}  // namespace
}  // namespace mongo
