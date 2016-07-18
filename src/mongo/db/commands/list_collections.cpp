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
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
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
            auto& elem = eqMatch->getData();
            if (elem.type() == String) {
                return {vector<StringData>{elem.valueStringData()}};
            } else {
                return vector<StringData>();
            }
        }
    } else if (matchType == MatchExpression::MATCH_IN) {
        auto matchIn = checked_cast<const InMatchExpression*>(matcher);
        if (matchIn->path() == "name" && matchIn->getRegexes().empty()) {
            vector<StringData> exactMatches;
            for (auto&& elem : matchIn->getEqualities()) {
                if (elem.type() == String) {
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
void _addWorkingSetMember(OperationContext* txn,
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

BSONObj buildViewBson(const ViewDefinition& view) {
    BSONObjBuilder b;
    b.append("name", view.name().coll());
    b.append("type", "view");
    BSONObj options = BSON("viewOn" << view.viewOn().coll() << "pipeline" << view.pipeline());
    b.append("options", options);
    BSONObj info = BSON("readOnly" << true);
    b.append("info", info);
    return b.obj();
}

BSONObj buildCollectionBson(OperationContext* txn, const Collection* collection) {

    if (!collection) {
        return {};
    }

    StringData collectionName = collection->ns().coll();
    if (collectionName == "system.namespaces") {
        return {};
    }

    BSONObjBuilder b;
    b.append("name", collectionName);
    b.append("type", "collection");

    CollectionOptions options = collection->getCatalogEntry()->getCollectionOptions(txn);
    b.append("options", options.toBSON());

    BSONObj info = BSON("readOnly" << storageGlobalParams.readOnly);
    b.append("info", info);

    return b.obj();
}

class CmdListCollections : public Command {
public:
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool slaveOverrideOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "list collections for this db";
    }

    virtual Status checkAuthForCommand(ClientBasic* client,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj) {
        AuthorizationSession* authzSession = AuthorizationSession::get(client);

        // Check for the listCollections ActionType on the database
        // or find on system.namespaces for pre 3.0 systems.
        if (authzSession->isAuthorizedForActionsOnResource(ResourcePattern::forDatabaseName(dbname),
                                                           ActionType::listCollections) ||
            authzSession->isAuthorizedForActionsOnResource(
                ResourcePattern::forExactNamespace(NamespaceString(dbname, "system.namespaces")),
                ActionType::find)) {
            return Status::OK();
        }

        return Status(ErrorCodes::Unauthorized,
                      str::stream() << "Not authorized to list collections on db: " << dbname);
    }

    CmdListCollections() : Command("listCollections") {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& jsobj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        unique_ptr<MatchExpression> matcher;
        BSONElement filterElt = jsobj["filter"];
        if (!filterElt.eoo()) {
            if (filterElt.type() != mongo::Object) {
                return appendCommandStatus(
                    result, Status(ErrorCodes::BadValue, "\"filter\" must be an object"));
            }
            // The collator is null because collection objects are compared using binary comparison.
            const CollatorInterface* collator = nullptr;
            StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
                filterElt.Obj(), ExtensionsCallbackDisallowExtensions(), collator);
            if (!statusWithMatcher.isOK()) {
                return appendCommandStatus(result, statusWithMatcher.getStatus());
            }
            matcher = std::move(statusWithMatcher.getValue());
        }

        const long long defaultBatchSize = std::numeric_limits<long long>::max();
        long long batchSize;
        Status parseCursorStatus =
            CursorRequest::parseCommandCursorOptions(jsobj, defaultBatchSize, &batchSize);
        if (!parseCursorStatus.isOK()) {
            return appendCommandStatus(result, parseCursorStatus);
        }

        ScopedTransaction scopedXact(txn, MODE_IS);
        AutoGetDb autoDb(txn, dbname, MODE_S);

        Database* db = autoDb.getDb();

        auto ws = make_unique<WorkingSet>();
        auto root = make_unique<QueuedDataStage>(txn, ws.get());

        if (db) {
            if (auto collNames = _getExactNameMatches(matcher.get())) {
                for (auto&& collName : *collNames) {
                    auto nss = NamespaceString(db->name(), collName);
                    Collection* collection = db->getCollection(nss);
                    BSONObj collBson = buildCollectionBson(txn, collection);
                    if (!collBson.isEmpty()) {
                        _addWorkingSetMember(txn, collBson, matcher.get(), ws.get(), root.get());
                    }
                }
            } else {
                for (auto&& collection : *db) {
                    BSONObj collBson = buildCollectionBson(txn, collection);
                    if (!collBson.isEmpty()) {
                        _addWorkingSetMember(txn, collBson, matcher.get(), ws.get(), root.get());
                    }
                }
            }
            auto viewCatalog = db->getViewCatalog();
            if (viewCatalog) {
                for (auto& view : *viewCatalog) {
                    BSONObj viewBson = buildViewBson(*(view.second.get()));
                    if (!viewBson.isEmpty()) {
                        _addWorkingSetMember(txn, viewBson, matcher.get(), ws.get(), root.get());
                    }
                }
            }
        }

        const std::string cursorNamespace = str::stream() << dbname << ".$cmd." << getName();
        dassert(NamespaceString(cursorNamespace).isValid());
        dassert(NamespaceString(cursorNamespace).isListCollectionsCursorNS());

        auto statusWithPlanExecutor = PlanExecutor::make(
            txn, std::move(ws), std::move(root), cursorNamespace, PlanExecutor::YIELD_MANUAL);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        BSONArrayBuilder firstBatch;

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

        CursorId cursorId = 0LL;
        if (!exec->isEOF()) {
            exec->saveState();
            exec->detachFromOperationContext();
            ClientCursor* cursor =
                new ClientCursor(CursorManager::getGlobalCursorManager(),
                                 exec.release(),
                                 cursorNamespace,
                                 txn->recoveryUnit()->isReadingFromMajorityCommittedSnapshot());
            cursorId = cursor->cursorid();
        }

        appendCursorResponseObject(cursorId, cursorNamespace, firstBatch.arr(), &result);

        return true;
    }
} cmdListCollections;

}  // namespace
}  // namespace mongo
