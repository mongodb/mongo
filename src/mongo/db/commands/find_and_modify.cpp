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

#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/projection.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/util/log.h"

namespace mongo {

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

            const std::string coll = cmdObj.firstElement().valuestrsafe();
            if (coll.empty()) {
                errmsg = "no collection name specified";
                return false;
            }

            const std::string ns = dbname + '.' + coll;

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
                if ( returnNew ) {
                    errmsg = "remove and returnNew can't co-exist";
                    return false;
                }
            }
            else if ( !cmdObj.hasField("update") ) {
                errmsg = "need remove or update";
                return false;
            }

            bool ok = false;
            int attempt = 0;
            while ( 1 ) {
                try {
                    errmsg = "";

                    // We can always retry because we only ever modify one document
                    ok = runImpl(txn,
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
                    break;
                }
                catch (const WriteConflictException&) {
                    if ( attempt++ > 1 ) {
                        log() << "got WriteConflictException on findAndModify for " << ns
                              <<  " retrying attempt: " << attempt;
                    }
                }
            }

            if ( !ok && errmsg == "no-collection" ) {
                // Take X lock so we can create collection, then re-run operation.
                ScopedTransaction transaction(txn, MODE_IX);
                Lock::DBLock lk(txn->lockState(), dbname, MODE_X);
                Client::Context ctx(txn, ns, false /* don't check version */);
                Database* db = ctx.db();
                if ( db->getCollection( ns ) ) {
                    // someone else beat us to it, that's ok
                    // we might race while we unlock if someone drops
                    // but that's ok, we'll just do nothing and error out
                }
                else {
                    WriteUnitOfWork wuow(txn);
                    uassertStatusOK( userCreateNS( txn, db,
                                                   ns, BSONObj(),
                                                   !fromRepl ) );
                    wuow.commit();
                }

                errmsg = "";
                ok = runImpl(txn,
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

            Client::WriteContext cx(txn, ns);
            Collection* collection = cx.getCollection();

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

            BSONObj doc;
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

                PlanExecutor::ExecState state = exec->getNext(&doc, NULL);
                if (PlanExecutor::ADVANCED == state) {
                    found = true;
                }
                else if (PlanExecutor::FAILURE == state || PlanExecutor::DEAD == state) {
                    if (PlanExecutor::FAILURE == state &&
                        WorkingSetCommon::isValidStatusMemberObject(doc)) {
                        const Status errorStatus = WorkingSetCommon::getMemberObjectStatus(doc);
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
                    deleteObjects(txn, cx.db(), ns, queryModified, PlanExecutor::YIELD_AUTO,
                                  true, true);
                    BSONObjBuilder le( result.subobjStart( "lastErrorObject" ) );
                    le.appendNumber( "n" , 1 );
                    le.done();
                }
            }
            else {
                // update
                if ( ! found && ! upsert ) {
                    // didn't have it, and am not upserting
                    _appendHelper(result, doc, found, fields, whereCallback);
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

                    request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

                    // TODO(greg) We need to send if we are ignoring
                    // the shard version below, but for now no
                    UpdateLifecycleImpl updateLifecycle(false, requestNs);
                    request.setLifecycle(&updateLifecycle);
                    UpdateResult res = mongo::update(txn,
                                                     cx.db(),
                                                     request,
                                                     &txn->getCurOp()->debug());

                    if ( !collection ) {
                        // collection created by an upsert
                        collection = cx.getCollection();
                    }

                    LOG(3) << "update result: "  << res ;
                    if ( returnNew ) {
                        if ( !res.upserted.isEmpty() ) {
                            BSONElement upsertedElem = res.upserted[kUpsertedFieldName];
                            LOG(3) << "using new _id to get new doc: "
                                   << upsertedElem;
                            queryModified = upsertedElem.wrap("_id");
                        }
                        else if ( queryModified["_id"].type() ) {
                            // we do this so that if the update changes the fields, it still matches
                            queryModified = queryModified["_id"].wrap();
                        }

                        LOG(3) << "using modified query to return the new doc: " << queryModified;
                        if ( ! Helpers::findOne( txn, collection, queryModified, doc ) ) {
                            errmsg = str::stream() << "can't find object after modification  " 
                                                   << " ns: " << ns 
                                                   << " queryModified: " << queryModified 
                                                   << " queryOriginal: " << query;
                            log() << errmsg << endl;
                            return false;
                        }
                        _appendHelper(result, doc, true, fields, whereCallback);
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
