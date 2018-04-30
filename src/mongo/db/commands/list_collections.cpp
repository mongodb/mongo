/**
 *    Copyright (C) 2014-2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/list_collections_filter.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/cursor_request.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

namespace {

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
            StringData name(eqMatch->getData().valuestrsafe());
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
                StringData name(elem.valuestrsafe());
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
 * Does not add any information about the system.namespaces collection, or non-existent collections.
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
    member->obj = Snapshotted<BSONObj>(SnapshotId(), maybe);
    member->transitionToOwnedObj();
    root->pushBack(id);
}

BSONObj buildViewBson(const ViewDefinition& view, bool nameOnly) {
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

/**
 * Return an object describing the collection. Takes a collection lock if nameOnly is false.
 */
BSONObj buildCollectionBson(OperationContext* opCtx,
                            const Collection* collection,
                            bool includePendingDrops,
                            bool nameOnly) {

    if (!collection) {
        return {};
    }

    auto nss = collection->ns();
    auto collectionName = nss.coll();
    if (collectionName == "system.namespaces") {
        return {};
    }

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

    Lock::CollectionLock clk(opCtx->lockState(), nss.ns(), MODE_IS);
    CollectionOptions options = collection->getCatalogEntry()->getCollectionOptions(opCtx);

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

class CmdListCollections : public BasicCommand {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    std::string help() const override {
        return "list collections for this db";
    }

    virtual Status checkAuthForCommand(Client* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) const {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        if (authzSession->isAuthorizedToListCollections(dbname)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list collections on db: " << dbname);
    }

    CmdListCollections() : BasicCommand("listCollections") {}

    bool run(OperationContext* opCtx,
             const string& dbname,
             const BSONObj& jsobj,
             BSONObjBuilder& result) {
        unique_ptr<MatchExpression> matcher;

        const bool nameOnly = jsobj["nameOnly"].trueValue();

        // Check for 'filter' argument.
        BSONElement filterElt = jsobj["filter"];
        if (!filterElt.eoo()) {
            if (filterElt.type() != mongo::Object) {
                uasserted(ErrorCodes::BadValue, "\"filter\" must be an object");
            }
            // The collator is null because collection objects are compared using binary comparison.
            const CollatorInterface* collator = nullptr;
            boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, collator));
            StatusWithMatchExpression statusWithMatcher =
                MatchExpressionParser::parse(filterElt.Obj(), std::move(expCtx));
            uassertStatusOK(statusWithMatcher.getStatus());
            matcher = std::move(statusWithMatcher.getValue());
        }

        const long long defaultBatchSize = std::numeric_limits<long long>::max();
        long long batchSize;
        Status parseCursorStatus =
            CursorRequest::parseCommandCursorOptions(jsobj, defaultBatchSize, &batchSize);
        uassertStatusOK(parseCursorStatus);

        // Check for 'includePendingDrops' flag. The default is to not include drop-pending
        // collections.
        bool includePendingDrops;
        Status status = bsonExtractBooleanFieldWithDefault(
            jsobj, "includePendingDrops", false, &includePendingDrops);
        uassertStatusOK(status);

        const NamespaceString cursorNss = NamespaceString::makeListCollectionsNSS(dbname);
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec;
        BSONArrayBuilder firstBatch;
        {
            AutoGetDb autoDb(opCtx, dbname, MODE_IS);
            Database* db = autoDb.getDb();

            auto ws = make_unique<WorkingSet>();
            auto root = make_unique<QueuedDataStage>(opCtx, ws.get());

            if (db) {
                if (auto collNames = _getExactNameMatches(matcher.get())) {
                    for (auto&& collName : *collNames) {
                        auto nss = NamespaceString(db->name(), collName);
                        Collection* collection = db->getCollection(opCtx, nss);
                        BSONObj collBson =
                            buildCollectionBson(opCtx, collection, includePendingDrops, nameOnly);
                        if (!collBson.isEmpty()) {
                            _addWorkingSetMember(
                                opCtx, collBson, matcher.get(), ws.get(), root.get());
                        }
                    }
                } else {
                    for (auto&& collection : *db) {
                        BSONObj collBson =
                            buildCollectionBson(opCtx, collection, includePendingDrops, nameOnly);
                        if (!collBson.isEmpty()) {
                            _addWorkingSetMember(
                                opCtx, collBson, matcher.get(), ws.get(), root.get());
                        }
                    }
                }

                // Skipping views is only necessary for internal cloning operations.
                bool skipViews = filterElt.type() == mongo::Object &&
                    SimpleBSONObjComparator::kInstance.evaluate(
                        filterElt.Obj() == ListCollectionsFilter::makeTypeCollectionFilter());
                if (!skipViews) {
                    db->getViewCatalog()->iterate(opCtx, [&](const ViewDefinition& view) {
                        BSONObj viewBson = buildViewBson(view, nameOnly);
                        if (!viewBson.isEmpty()) {
                            _addWorkingSetMember(
                                opCtx, viewBson, matcher.get(), ws.get(), root.get());
                        }
                    });
                }
            }

            exec = uassertStatusOK(PlanExecutor::make(
                opCtx, std::move(ws), std::move(root), cursorNss, PlanExecutor::NO_YIELD));

            for (long long objCount = 0; objCount < batchSize; objCount++) {
                BSONObj next;
                PlanExecutor::ExecState state = exec->getNext(&next, NULL);
                if (state == PlanExecutor::IS_EOF) {
                    break;
                }
                invariant(state == PlanExecutor::ADVANCED);

                // If we can't fit this result inside the current batch, then we stash it for later.
                if (!FindCommon::haveSpaceForNext(next, objCount, firstBatch.len())) {
                    exec->enqueue(next);
                    break;
                }

                firstBatch.append(next);
            }
            if (exec->isEOF()) {
                appendCursorResponseObject(0LL, cursorNss.ns(), firstBatch.arr(), &result);
                return true;
            }
            exec->saveState();
            exec->detachFromOperationContext();
        }  // Drop db lock. Global cursor registration must be done without holding any locks.

        auto pinnedCursor = CursorManager::getGlobalCursorManager()->registerCursor(
            opCtx,
            {std::move(exec),
             cursorNss,
             AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
             opCtx->recoveryUnit()->getReadConcernLevel(),
             jsobj});

        appendCursorResponseObject(
            pinnedCursor.getCursor()->cursorid(), cursorNss.ns(), firstBatch.arr(), &result);

        return true;
    }
} cmdListCollections;

}  // namespace
}  // namespace mongo
