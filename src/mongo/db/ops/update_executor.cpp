/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/ops/update_executor.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

    namespace {

        // TODO: Make this a function on NamespaceString, or make it cleaner.
        inline void validateUpdate(const char* ns ,
                                   const BSONObj& updateobj,
                                   const BSONObj& patternOrig) {
            uassert(10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0);
            if (strstr(ns, ".system.")) {
                /* dm: it's very important that system.indexes is never updated as IndexDetails
                   has pointers into it */
                uassert(10156,
                         str::stream() << "cannot update system collection: "
                         << ns << " q: " << patternOrig << " u: " << updateobj,
                         legalClientSystemNS(ns , true));
            }
        }

    } // namespace

    UpdateExecutor::UpdateExecutor(const UpdateRequest* request, OpDebug* opDebug) :
        _request(request),
        _opDebug(opDebug),
        _driver(UpdateDriver::Options()),
        _canonicalQuery(),
        _isQueryParsed(false),
        _isUpdateParsed(false) {
    }

    UpdateExecutor::~UpdateExecutor() {}

    Status UpdateExecutor::prepare() {
        // We parse the update portion before the query portion because the dispostion of the update
        // may determine whether or not we need to produce a CanonicalQuery at all.  For example, if
        // the update involves the positional-dollar operator, we must have a CanonicalQuery even if
        // it isn't required for query execution.
        Status status = parseUpdate();
        if (!status.isOK())
            return status;
        status = parseQuery();
        if (!status.isOK())
            return status;
        return Status::OK();
    }

    PlanExecutor* UpdateExecutor::getPlanExecutor() {
        return _exec.get();
    }

    MONGO_FP_DECLARE(implicitCollectionCreationDelay);

    Status UpdateExecutor::prepareInLock(Database* db) {
        // If we have a non-NULL PlanExecutor, then we've already done the in-lock preparation.
        if (_exec.get()) {
            return Status::OK();
        }

        const NamespaceString& nsString = _request->getNamespaceString();
        UpdateLifecycle* lifecycle = _request->getLifecycle();

        validateUpdate(nsString.ns().c_str(), _request->getUpdates(), _request->getQuery());

        Collection* collection = db->getCollection(_request->getOpCtx(), nsString.ns());

        // The update stage does not create its own collection.  As such, if the update is
        // an upsert, create the collection that the update stage inserts into beforehand.
        if (!collection && _request->isUpsert()) {
            OperationContext* const txn = _request->getOpCtx();

            // We have to have an exclsive lock on the db to be allowed to create the collection.
            // Callers should either get an X or create the collection.
            const Locker* locker = txn->lockState();
            invariant( locker->isW() ||
                       locker->isLockHeldForMode( ResourceId( RESOURCE_DATABASE, nsString.db() ),
                                                  MODE_X ) );

            Lock::DBLock lk(txn->lockState(), nsString.db(), MODE_X);

            WriteUnitOfWork wuow(txn);
            invariant(db->createCollection(txn, nsString.ns()));

            if (!_request->isFromReplication()) {
                repl::logOp(txn,
                            "c",
                            (db->name() + ".$cmd").c_str(),
                            BSON("create" << (nsString.coll())));
            }
            wuow.commit();
            collection = db->getCollection(_request->getOpCtx(), nsString.ns());
            invariant(collection);
        }

        // TODO: This seems a bit circuitious.
        _opDebug->updateobj = _request->getUpdates();

        // If this is a user-issued update, then we want to return an error: you cannot perform
        // writes on a secondary. If this is an update to a secondary from the replication system,
        // however, then we make an exception and let the write proceed. In this case,
        // shouldCallLogOp() will be false.
        if (_request->shouldCallLogOp() &&
            !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(nsString.db())) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while performing update on "
                                        << nsString.ns());
        }

        if (lifecycle) {
            lifecycle->setCollection(collection);
            _driver.refreshIndexKeys(lifecycle->getIndexKeys(_request->getOpCtx()));
        }

        PlanExecutor* rawExec = NULL;
        Status getExecStatus = Status::OK();
        if (_canonicalQuery.get()) {
            // This is the regular path for when we have a CanonicalQuery.
            getExecStatus = getExecutorUpdate(_request->getOpCtx(), db, _canonicalQuery.release(),
                                              _request, &_driver, _opDebug, &rawExec);
        }
        else {
            // This is the idhack fast-path for getting a PlanExecutor without doing the work
            // to create a CanonicalQuery.
            getExecStatus = getExecutorUpdate(_request->getOpCtx(), db, nsString.ns(), _request,
                                              &_driver, _opDebug, &rawExec);
        }

        if (!getExecStatus.isOK()) {
            return getExecStatus;
        }

        invariant(rawExec);
        _exec.reset(rawExec);

        // If yielding is allowed for this plan, then set an auto yield policy. Otherwise set
        // a manual yield policy.
        const bool canYield = !_request->isGod() && (
            _canonicalQuery.get() ?
            !QueryPlannerCommon::hasNode(_canonicalQuery->root(), MatchExpression::ATOMIC) :
            !LiteParsedQuery::isQueryIsolated(_request->getQuery()));

        PlanExecutor::YieldPolicy policy = canYield ? PlanExecutor::YIELD_AUTO :
                                                      PlanExecutor::YIELD_MANUAL;

        _exec->setYieldPolicy(policy);

        return Status::OK();
    }

    UpdateResult UpdateExecutor::execute(Database* db) {
        uassertStatusOK(prepare());

        LOG(3) << "processing update : " << *_request;

        // If we've already done the in-lock preparation, this is a no-op.
        Status status = prepareInLock(db);
        uassert(17243,
                "could not get executor " + _request->getQuery().toString()
                                         + "; " + causedBy(status),
                status.isOK());

        // Run the plan (don't need to collect results because UpdateStage always returns
        // NEED_TIME).
        uassertStatusOK(_exec->executePlan());

        // Get stats from the root stage.
        invariant(_exec->getRootStage()->stageType() == STAGE_UPDATE);
        UpdateStage* updateStage = static_cast<UpdateStage*>(_exec->getRootStage());
        const UpdateStats* updateStats =
            static_cast<const UpdateStats*>(updateStage->getSpecificStats());

        // Use stats from the root stage to fill out opDebug.
        _opDebug->nMatched = updateStats->nMatched;
        _opDebug->nModified = updateStats->nModified;
        _opDebug->upsert = updateStats->inserted;
        _opDebug->fastmodinsert = updateStats->fastmodinsert;
        _opDebug->fastmod = updateStats->fastmod;

        // Historically, 'opDebug' considers 'nMatched' and 'nModified' to be 1 (rather than 0) if
        // there is an upsert that inserts a document. The UpdateStage does not participate in this
        // madness in order to have saner stats reporting for explain. This means that we have to
        // set these values "manually" in the case of an insert.
        if (updateStats->inserted) {
            _opDebug->nMatched = 1;
            _opDebug->nModified = 1;
        }

        // Get summary information about the plan.
        PlanSummaryStats stats;
        Explain::getSummaryStats(_exec.get(), &stats);
        _opDebug->nscanned = stats.totalKeysExamined;
        _opDebug->nscannedObjects = stats.totalDocsExamined;

        return UpdateResult(updateStats->nMatched > 0 /* Did we update at least one obj? */,
                            !_driver.isDocReplacement() /* $mod or obj replacement */,
                            _opDebug->nModified /* number of modified docs, no no-ops */,
                            _opDebug->nMatched /* # of docs matched/updated, even no-ops */,
                            updateStats->objInserted);
    }

    Status UpdateExecutor::parseQuery() {
        if (_isQueryParsed)
            return Status::OK();

        dassert(!_canonicalQuery.get());
        dassert(_isUpdateParsed);

        if (!_driver.needMatchDetails() && CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
            _isQueryParsed = true;
            return Status::OK();
        }

        CanonicalQuery* cqRaw;
        const WhereCallbackReal whereCallback(
                                    _request->getOpCtx(), _request->getNamespaceString().db());

        Status status = CanonicalQuery::canonicalize(_request->getNamespaceString().ns(),
                                                     _request->getQuery(),
                                                     _request->isExplain(),
                                                     &cqRaw,
                                                     whereCallback);
        if (status.isOK()) {
            _canonicalQuery.reset(cqRaw);
            _isQueryParsed = true;
        }

        return status;
    }

    Status UpdateExecutor::parseUpdate() {
        if (_isUpdateParsed)
            return Status::OK();

        const NamespaceString& ns(_request->getNamespaceString());

        // Should the modifiers validate their embedded docs via okForStorage
        // Only user updates should be checked. Any system or replication stuff should pass through.
        // Config db docs shouldn't get checked for valid field names since the shard key can have
        // a dot (".") in it.
        const bool shouldValidate = !(_request->isFromReplication() ||
                                      ns.isConfigDB() ||
                                      _request->isFromMigration());

        _driver.setLogOp(true);
        _driver.setModOptions(ModifierInterface::Options(_request->isFromReplication(),
                                                         shouldValidate));
        Status status = _driver.parse(_request->getUpdates(), _request->isMulti());
        if (status.isOK())
            _isUpdateParsed = true;
        return status;
    }

}  // namespace mongo
