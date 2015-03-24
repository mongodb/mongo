/**
 *    Copyright (C) 2012-2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands/find_and_modify.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/projection.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/write_concern.h"
#include "mongo/util/log.h"

namespace mongo {

    using boost::scoped_ptr;
    using std::endl;
    using std::string;
    using std::stringstream;

    /* Find and Modify an object returning either the old (default) or new value*/
    class CmdFindAndModify : public Command {
    public:
        virtual void help( stringstream &help ) const {
            help <<
                 "{ findAndModify: \"collection\", query: {processed:false}, update: {$set: {processed:true}}, new: true}\n"
                 "{ findAndModify: \"collection\", query: {processed:false}, remove: true, sort: {priority:-1}}\n"
                 "Either update or remove is required, all other fields have default values.\n"
                 "Output is in the \"value\" field\n";
        }

        CmdFindAndModify() : Command("findAndModify", false, "findandmodify") { }
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return true; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            find_and_modify::addPrivilegesRequiredForFindAndModify(this, dbname, cmdObj, out);
        }

        virtual bool run(OperationContext* txn,
                         const string& dbname,
                         BSONObj& cmdObj,
                         int options,
                         string& errmsg,
                         BSONObjBuilder& result,
                         bool fromRepl) {

            const std::string ns = parseNsCollectionRequired(dbname, cmdObj);

            const BSONObj query = cmdObj.getObjectField("query");
            const BSONObj fields = cmdObj.getObjectField("fields");
            const BSONObj update = cmdObj.getObjectField("update");
            const BSONObj sort = cmdObj.getObjectField("sort");
            
            bool upsert = cmdObj["upsert"].trueValue();
            bool returnNew = cmdObj["new"].trueValue();
            bool remove = cmdObj["remove"].trueValue();

            if ( remove ) {
                if ( upsert ) {
                    errmsg = "remove and upsert can't co-exist";
                    return false;
                }
                if ( !update.isEmpty() ) {
                    errmsg = "remove and update can't co-exist";
                    return false;
                }
                if ( returnNew ) {
                    errmsg = "remove and returnNew can't co-exist";
                    return false;
                }
            }
            else if ( !cmdObj.hasField("update") ) {
                errmsg = "need remove or update";
                return false;
            }

            StatusWith<WriteConcernOptions> wcResult = extractWriteConcern(cmdObj);
            if (!wcResult.isOK()) {
                return appendCommandStatus(result, wcResult.getStatus());
            }
            txn->setWriteConcern(wcResult.getValue());
            setupSynchronousCommit(txn);

            bool ok = false;
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                errmsg = "";

                // We can always retry because we only ever modify one document
                ok = runImpl(txn,
                             dbname,
                             ns,
                             query,
                             fields,
                             update,
                             sort,
                             upsert,
                             returnNew,
                             remove,
                             result,
                             errmsg);
            } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "findAndModify", ns);

            if ( !ok && errmsg == "no-collection" ) {
                // Take X lock so we can create collection, then re-run operation.
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock lk(txn->lockState(), dbname, MODE_X);
                OldClientContext ctx(txn, ns, false /* don't check version */);
                if (!fromRepl &&
                    !repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname)) {
                    return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                        << "Not primary while creating collection " << ns
                        << " during findAndModify"));
                }
                Database* db = ctx.db();
                if ( db->getCollection( ns ) ) {
                    // someone else beat us to it, that's ok
                    // we might race while we unlock if someone drops
                    // but that's ok, we'll just do nothing and error out
                }
                else {
                    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                        WriteUnitOfWork wuow(txn);
                        uassertStatusOK( userCreateNS( txn, db,
                                                       ns, BSONObj(),
                                                       !fromRepl ) );
                        wuow.commit();
                    } MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "findAndModify", ns);
                }

                errmsg = "";
                ok = runImpl(txn,
                             dbname,
                             ns,
                             query,
                             fields,
                             update,
                             sort,
                             upsert,
                             returnNew,
                             remove,
                             result,
                             errmsg);
            }

            WriteConcernResult res;
            Status waitStatus = waitForWriteConcern(txn, txn->getClient()->getLastOp(), &res);
            appendCommandWCStatus(result, waitStatus);

            return ok;
        }

        static void _appendHelper(BSONObjBuilder& result,
                                  const BSONObj& doc,
                                  bool found,
                                  const BSONObj& fields,
                                  const MatchExpressionParser::WhereCallback& whereCallback) {
            if ( ! found ) {
                result.appendNull( "value" );
                return;
            }

            if ( fields.isEmpty() ) {
                result.append( "value" , doc );
                return;
            }

            Projection p;
            p.init(fields, whereCallback);
            result.append( "value" , p.transform( doc ) );
        }

        static bool runImpl(OperationContext* txn,
                            const string& dbname,
                            const string& ns,
                            const BSONObj& query,
                            const BSONObj& fields,
                            const BSONObj& update,
                            const BSONObj& sort,
                            bool upsert,
                            bool returnNew,
                            bool remove ,
                            BSONObjBuilder& result,
                            string& errmsg) {

            AutoGetOrCreateDb autoDb(txn, dbname, MODE_IX);
            Lock::CollectionLock collLock(txn->lockState(), ns, MODE_IX);
            OldClientContext ctx(txn, ns, autoDb.getDb(), autoDb.justCreated());

            if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(dbname)) {
                return appendCommandStatus(result, Status(ErrorCodes::NotMaster, str::stream()
                    << "Not primary while running findAndModify in " << ns));
            }

            Collection* collection = ctx.db()->getCollection(ns);

            const WhereCallbackReal whereCallback(txn, StringData(ns));

            if ( !collection ) {
                if ( !upsert ) {
                    // no collectio and no upsert, so can't possible do anything
                    _appendHelper( result, BSONObj(), false, fields, whereCallback );
                    return true;
                }
                // no collection, but upsert, so we want to create it
                // problem is we only have IX on db and collection :(
                // so we tell our caller who can do it
                errmsg = "no-collection";
                return false;
            }

            Snapshotted<BSONObj> snapshotDoc;
            RecordId loc;
            bool found = false;
            {
                CanonicalQuery* cq;
                const BSONObj projection;
                const long long skip = 0;
                const long long limit = -1; // 1 document requested; negative indicates hard limit.
                uassertStatusOK(CanonicalQuery::canonicalize(ns,
                                                             query,
                                                             sort,
                                                             projection,
                                                             skip,
                                                             limit,
                                                             &cq,
                                                             whereCallback));

                PlanExecutor* rawExec;
                uassertStatusOK(getExecutor(txn,
                                            collection,
                                            cq,
                                            PlanExecutor::YIELD_AUTO,
                                            &rawExec,
                                            QueryPlannerParams::DEFAULT));

                scoped_ptr<PlanExecutor> exec(rawExec);

                PlanExecutor::ExecState state = exec->getNextSnapshotted(&snapshotDoc, &loc);
                if (PlanExecutor::ADVANCED == state) {
                    found = true;
                }
                else if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
                    if (PlanExecutor::FAILURE == state &&
                        WorkingSetCommon::isValidStatusMemberObject(snapshotDoc.value())) {
                        const Status errorStatus =
                            WorkingSetCommon::getMemberObjectStatus(snapshotDoc.value());
                        invariant(!errorStatus.isOK());
                        uasserted(errorStatus.code(), errorStatus.reason());
                    }
                    uasserted(ErrorCodes::OperationFailed,
                              str::stream() << "executor returned " << PlanExecutor::statestr(state)
                                            << " while finding document to update");
                }
                else {
                    invariant(PlanExecutor::IS_EOF == state);
                }
            }

            WriteUnitOfWork wuow(txn);
            if (found) {
                // We found a doc, but it might not be associated with the active snapshot.
                // If the doc has changed or is no longer in the collection, we will throw a
                // write conflict exception and start again from the beginning.
                if (txn->recoveryUnit()->getSnapshotId() != snapshotDoc.snapshotId()) {
                    BSONObj oldObj = snapshotDoc.value();
                    if (!collection->findDoc(txn, loc, &snapshotDoc)) {
                        // Got deleted in the new snapshot.
                        throw WriteConflictException();
                    }

                    if (!oldObj.binaryEqual(snapshotDoc.value())) {
                        // Got updated in the new snapshot.
                        throw WriteConflictException();
                    }
                }

                // If we get here without throwing, then we should have the copy of the doc from
                // the latest snapshot.
                invariant(txn->recoveryUnit()->getSnapshotId() == snapshotDoc.snapshotId());
            }

            BSONObj doc = snapshotDoc.value();
            BSONObj queryModified = query;
            if (found && !doc["_id"].eoo() && !CanonicalQuery::isSimpleIdQuery(query)) {
                // we're going to re-write the query to be more efficient
                // we have to be a little careful because of positional operators
                // maybe we can pass this all through eventually, but right now isn't an easy way
                
                bool hasPositionalUpdate = false;
                {
                    // if the update has a positional piece ($)
                    // then we need to pull all query parts in
                    // so here we check for $
                    // a little hacky
                    BSONObjIterator i( update );
                    while ( i.more() ) {
                        const BSONElement& elem = i.next();
                        
                        if ( elem.fieldName()[0] != '$' || elem.type() != Object )
                            continue;

                        BSONObjIterator j( elem.Obj() );
                        while ( j.more() ) {
                            if ( str::contains( j.next().fieldName(), ".$" ) ) {
                                hasPositionalUpdate = true;
                                break;
                            }
                        }
                    }
                }

                BSONObjBuilder b(query.objsize() + 10);
                b.append( doc["_id"] );
                
                bool addedAtomic = false;

                BSONObjIterator i(query);
                while ( i.more() ) {
                    const BSONElement& elem = i.next();

                    if ( str::equals( "_id" , elem.fieldName() ) ) {
                        // we already do _id
                        continue;
                    }
                    
                    if ( ! hasPositionalUpdate ) {
                        // if there is a dotted field, accept we may need more query parts
                        continue;
                    }
                    
                    if ( ! addedAtomic ) {
                        b.appendBool( "$atomic" , true );
                        addedAtomic = true;
                    }

                    b.append( elem );
                }

                queryModified = b.obj();
            }

            if ( remove ) {
                _appendHelper(result, doc, found, fields, whereCallback);
                if ( found ) {
                    deleteObjects(txn, ctx.db(), ns, queryModified, PlanExecutor::YIELD_MANUAL,
                                  true, true);

                    // Committing the WUOW can close the current snapshot. Until this happens, the
                    // snapshot id should not have changed.
                    invariant(txn->recoveryUnit()->getSnapshotId() == snapshotDoc.snapshotId());

                    // Must commit the write before doing anything that could throw.
                    wuow.commit();

                    BSONObjBuilder le( result.subobjStart( "lastErrorObject" ) );
                    le.appendNumber( "n" , 1 );
                    le.done();
                }
            }
            else {
                // update
                if (!found) {
                    if (!upsert) {
                        // Didn't have it, and not upserting.
                        _appendHelper(result, doc, found, fields, whereCallback);
                    }
                    else {
                        // Do an insert.
                        BSONObj newDoc;
                        {
                            CanonicalQuery* rawCq;
                            uassertStatusOK(CanonicalQuery::canonicalize(ns, queryModified, &rawCq,
                                                                         WhereCallbackNoop()));
                            boost::scoped_ptr<CanonicalQuery> cq(rawCq);

                            UpdateDriver::Options opts;
                            UpdateDriver driver(opts);
                            uassertStatusOK(driver.parse(update));

                            mutablebson::Document doc(newDoc,
                                                      mutablebson::Document::kInPlaceDisabled);

                            const bool ignoreVersion = false;
                            UpdateLifecycleImpl updateLifecycle(ignoreVersion, collection->ns());

                            UpdateStats stats;
                            const bool isInternalRequest = false;

                            uassertStatusOK(UpdateStage::applyUpdateOpsForInsert(cq.get(),
                                                                                 queryModified,
                                                                                 &driver,
                                                                                 &updateLifecycle,
                                                                                 &doc,
                                                                                 isInternalRequest,
                                                                                 &stats,
                                                                                 &newDoc));
                        }

                        const bool enforceQuota = true;
                        uassertStatusOK(collection->insertDocument(txn, newDoc, enforceQuota)
                                        .getStatus());

                        // This is the last thing we do before the WriteUnitOfWork commits (except
                        // for some BSON manipulation).
                        getGlobalEnvironment()->getOpObserver()->onInsert(txn,
                                                                          collection->ns().ns(),
                                                                          newDoc); 

                        // Must commit the write and logOp() before doing anything that could throw.
                        wuow.commit();

                        // The third argument indicates whether or not we have something for the
                        // 'value' field returned by a findAndModify command.
                        //
                        // Since we did an insert, we have a doc only if the user asked us to
                        // return the new copy. We return a value of 'null' if we inserted and
                        // the user asked for the old copy.
                        _appendHelper(result, newDoc, returnNew, fields, whereCallback);

                        BSONObjBuilder le(result.subobjStart("lastErrorObject"));
                        le.appendBool("updatedExisting", false);
                        le.appendNumber("n", 1);
                        le.appendAs(newDoc["_id"], kUpsertedFieldName);
                        le.done();
                    }
                }
                else {
                    // we found it or we're updating

                    if ( ! returnNew ) {
                        _appendHelper(result, doc, found, fields, whereCallback);
                    }

                    const NamespaceString requestNs(ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(queryModified);
                    request.setUpdates(update);
                    request.setUpsert(upsert);
                    request.setUpdateOpLog();
                    request.setStoreResultDoc(returnNew);

                    request.setYieldPolicy(PlanExecutor::YIELD_MANUAL);

                    // TODO(greg) We need to send if we are ignoring
                    // the shard version below, but for now no
                    UpdateLifecycleImpl updateLifecycle(false, requestNs);
                    request.setLifecycle(&updateLifecycle);
                    UpdateResult res = mongo::update(txn,
                                                     ctx.db(),
                                                     request,
                                                     &txn->getCurOp()->debug());

                    invariant(collection);
                    invariant(res.existing);
                    LOG(3) << "update result: "  << res;

                    // Committing the WUOW can close the current snapshot. Until this happens, the
                    // snapshot id should not have changed.
                    invariant(txn->recoveryUnit()->getSnapshotId() == snapshotDoc.snapshotId());

                    // Must commit the write before doing anything that could throw.
                    wuow.commit();

                    if (returnNew) {
                        dassert(!res.newObj.isEmpty());
                        _appendHelper(result, res.newObj, true, fields, whereCallback);
                    }

                    BSONObjBuilder le( result.subobjStart( "lastErrorObject" ) );
                    le.appendBool( "updatedExisting" , res.existing );
                    le.appendNumber( "n" , res.numMatched );
                    if ( !res.upserted.isEmpty() ) {
                        le.append( res.upserted[kUpsertedFieldName] );
                    }
                    le.done();
                }
            }

            return true;
        }

    } cmdFindAndModify;

}
