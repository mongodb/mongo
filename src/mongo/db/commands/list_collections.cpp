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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include <memory>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/logv2/log.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

namespace {

// Failpoint which causes to hang "listCollections" cmd after acquiring the DB lock.
MONGO_FAIL_POINT_DEFINE(hangBeforeListCollections);

/**
 * Determines if 'matcher' is an exact match on the "name" field. If so, returns a vector of all the
 * collection names it is matching against. Returns {} if there is no obvious exact match on name.
 *
 * Note the collection names returned are not guaranteed to exist, nor are they guaranteed to match
 * 'matcher'.
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
 * allocates a WorkingSetMember containing information about 'collection', and adds it to 'root'.
 *
 * Does not add any information about non-existent collections.
 */
void _addWorkingSetMember(OperationContext* opCtx,
                          const BSONObj& maybe,
                          const MatchExpression* matcher,
                          WorkingSet* ws,
                          QueuedDataStage* root) {
    if (matcher && !matcher->matchesBSON(maybe)) {
        return;
    }

    WorkingSetID id = ws->allocate();
    WorkingSetMember* member = ws->get(id);
    member->keyData.clear();
    member->recordId = RecordId();
    member->resetDocument(SnapshotId(), maybe);
    member->transitionToOwnedObj();
    root->pushBack(id);
}

BSONObj buildViewBson(const ViewDefinition& view, bool nameOnly) {
    BSONObjBuilder b;
    b.append("name", view.name().coll());
    b.append("type", view.timeseries() ? "timeseries" : "view");

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

    BSONObj info = BSON("readOnly" << !view.timeseries());
    b.append("info", info);
    return b.obj();
}

/**
 * Return an object describing the collection. Takes a collection lock if nameOnly is false.
 */
BSONObj buildCollectionBson(OperationContext* opCtx,
                            const CollectionPtr& collection,
                            bool includePendingDrops,
                            bool nameOnly) {
    if (!collection) {
        return {};
    }
    auto nss = collection->ns();
    auto collectionName = nss.coll();

    // Drop-pending collections are replicated collections that have been marked for deletion.
    // These collections are considered dropped and should not be returned in the results for this
    // command, unless specified explicitly by the 'includePendingDrops' command argument.
    if (nss.isDropPendingNamespace() && !includePendingDrops) {
        return {};
    }

    BSONObjBuilder b;
    b.append("name", collectionName);
    b.append("type", "collection");

    if (nameOnly) {
        return b.obj();
    }

    CollectionOptions options =
        DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, collection->getCatalogId());

    // While the UUID is stored as a collection option, from the user's perspective it is an
    // unsettable read-only property, so put it in the 'info' section.
    auto uuid = options.uuid;
    options.uuid.reset();
    b.append("options", options.toBSON());

    BSONObjBuilder infoBuilder;
    infoBuilder.append("readOnly", storageGlobalParams.readOnly);
    if (uuid)
        infoBuilder.appendElements(uuid->toBSON());
    b.append("info", infoBuilder.obj());

    auto idIndex = collection->getIndexCatalog()->findIdIndex(opCtx);
    if (idIndex) {
        b.append("idIndex", idIndex->infoObj());
    }

    return b.obj();
}

void appendListCollectionsCursorReply(CursorId cursorId,
                                      const NamespaceString& cursorNss,
                                      std::vector<mongo::ListCollectionsReplyItem>&& firstBatch,
                                      BSONObjBuilder& result) {
    auto reply = ListCollectionsReply(
        ListCollectionsReplyCursor(cursorId, cursorNss, std::move(firstBatch)));
    reply.serialize(&result);
}

class CmdListCollections : public BasicCommand {
public:
    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
    }
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const final {
        return AllowedOnSecondary::kOptIn;
    }
    bool maintenanceOk() const override {
        return false;
    }
    bool adminOnly() const final {
        return false;
    }
    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return false;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    std::string help() const final {
        return "list collections for this db";
    }

    Status checkAuthForCommand(Client* client,
                               const std::string& dbname,
                               const BSONObj& cmdObj) const final {

        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        if (authzSession->checkAuthorizedToListCollections(dbname, cmdObj).isOK()) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list collections on db: " << dbname);
    }

    CmdListCollections() : BasicCommand("listCollections") {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) final {
        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        unique_ptr<MatchExpression> matcher;
        const auto as = AuthorizationSession::get(opCtx->getClient());

        auto parsed = ListCollections::parse(
            IDLParserErrorContext("listCollections",
                                  APIParameters::get(opCtx).getAPIStrict().value_or(false)),
            jsobj);
        const bool nameOnly = parsed.getNameOnly();
        const bool authorizedCollections = parsed.getAuthorizedCollections();

        // The collator is null because collection objects are compared using binary comparison.
        auto expCtx = make_intrusive<ExpressionContext>(
            opCtx, std::unique_ptr<CollatorInterface>(nullptr), NamespaceString(dbname));

        if (parsed.getFilter()) {
            matcher = uassertStatusOK(MatchExpressionParser::parse(*parsed.getFilter(), expCtx));
        }

        // Check for 'includePendingDrops' flag. The default is to not include drop-pending
        // collections.
        bool includePendingDrops;
        Status status = bsonExtractBooleanFieldWithDefault(
            jsobj, "includePendingDrops", false, &includePendingDrops);
        uassertStatusOK(status);

        const NamespaceString cursorNss = NamespaceString::makeListCollectionsNSS(dbname);
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        std::vector<mongo::ListCollectionsReplyItem> firstBatch;
        {
            AutoGetDb autoDb(opCtx, dbname, MODE_IS);
            Database* db = autoDb.getDb();

            CurOpFailpointHelpers::waitWhileFailPointEnabled(&hangBeforeListCollections,
                                                             opCtx,
                                                             "hangBeforeListCollections",
                                                             []() {},
                                                             false,
                                                             cursorNss);

            auto ws = std::make_unique<WorkingSet>();
            auto root = std::make_unique<QueuedDataStage>(expCtx.get(), ws.get());

            if (db) {
                if (auto collNames = _getExactNameMatches(matcher.get())) {
                    for (auto&& collName : *collNames) {
                        auto nss = NamespaceString(db->name(), collName);

                        // Only validate on a per-collection basis if the user requested
                        // a list of authorized collections
                        if (authorizedCollections &&
                            (!as->isAuthorizedForAnyActionOnResource(
                                ResourcePattern::forExactNamespace(nss)))) {
                            continue;
                        }

                        Lock::CollectionLock clk(opCtx, nss, MODE_IS);
                        CollectionPtr collection =
                            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
                        BSONObj collBson =
                            buildCollectionBson(opCtx, collection, includePendingDrops, nameOnly);
                        if (!collBson.isEmpty()) {
                            _addWorkingSetMember(
                                opCtx, collBson, matcher.get(), ws.get(), root.get());
                        }
                    }
                } else {
                    mongo::catalog::forEachCollectionFromDb(
                        opCtx, dbname, MODE_IS, [&](const CollectionPtr& collection) {
                            if (authorizedCollections &&
                                (!as->isAuthorizedForAnyActionOnResource(
                                    ResourcePattern::forExactNamespace(collection->ns())))) {
                                return true;
                            }
                            BSONObj collBson = buildCollectionBson(
                                opCtx, collection, includePendingDrops, nameOnly);
                            if (!collBson.isEmpty()) {
                                _addWorkingSetMember(
                                    opCtx, collBson, matcher.get(), ws.get(), root.get());
                            }
                            return true;
                        });
                }

                // Skipping views is only necessary for internal cloning operations.
                bool skipViews = parsed.getFilter() &&
                    SimpleBSONObjComparator::kInstance.evaluate(
                        *parsed.getFilter() == ListCollectionsFilter::makeTypeCollectionFilter());

                if (!skipViews) {
                    ViewCatalog::get(db)->iterate(opCtx, [&](const ViewDefinition& view) {
                        if (authorizedCollections &&
                            !as->isAuthorizedForAnyActionOnResource(
                                ResourcePattern::forExactNamespace(view.name()))) {
                            return;
                        }

                        BSONObj viewBson = buildViewBson(view, nameOnly);
                        if (!viewBson.isEmpty()) {
                            _addWorkingSetMember(
                                opCtx, viewBson, matcher.get(), ws.get(), root.get());
                        }
                    });
                }
            }

            exec =
                uassertStatusOK(plan_executor_factory::make(expCtx,
                                                            std::move(ws),
                                                            std::move(root),
                                                            &CollectionPtr::null,
                                                            PlanYieldPolicy::YieldPolicy::NO_YIELD,
                                                            cursorNss));

            long long batchSize = std::numeric_limits<long long>::max();
            if (parsed.getCursor() && parsed.getCursor()->getBatchSize()) {
                batchSize = *parsed.getCursor()->getBatchSize();
            }

            int bytesBuffered = 0;
            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj nextDoc;
                PlanExecutor::ExecState state = exec->getNext(&nextDoc, nullptr);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);

                // If we can't fit this result inside the current batch, then we stash it for later.
                if (!FindCommon::haveSpaceForNext(nextDoc, objCount, bytesBuffered)) {
                    exec->enqueue(nextDoc);
                    break;
                }

                try {
                    firstBatch.push_back(ListCollectionsReplyItem::parse(
                        IDLParserErrorContext("ListCollectionsReplyItem"), nextDoc));
                } catch (const DBException& exc) {
                    LOGV2_ERROR(5254300,
                                "Could not parse catalog entry while replying to listCollections",
                                "entry"_attr = nextDoc,
                                "error"_attr = exc);
                    fassertFailed(5254301);
                }
                bytesBuffered += nextDoc.objsize();
            }
            if (exec->isEOF()) {
                appendListCollectionsCursorReply(
                    0 /* cursorId */, cursorNss, std::move(firstBatch), result);
                return true;
            }
            exec->saveState();
            exec->detachFromOperationContext();
        }  // Drop db lock. Global cursor registration must be done without holding any locks.

        auto pinnedCursor = CursorManager::get(opCtx)->registerCursor(
            opCtx,
            {std::move(exec),
             cursorNss,
             AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
             APIParameters::get(opCtx),
             opCtx->getWriteConcern(),
             repl::ReadConcernArgs::get(opCtx),
             jsobj,
             uassertStatusOK(AuthorizationSession::get(opCtx->getClient())
                                 ->checkAuthorizedToListCollections(dbname, jsobj))});

        pinnedCursor->incNBatches();
        pinnedCursor->incNReturnedSoFar(firstBatch.size());

        appendListCollectionsCursorReply(
            pinnedCursor.getCursor()->cursorid(), cursorNss, std::move(firstBatch), result);
        return true;
    }
} cmdListCollections;

}  // namespace
}  // namespace mongo
