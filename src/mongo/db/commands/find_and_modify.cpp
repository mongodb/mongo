// find_and_modify.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommands

#include "mongo/platform/basic.h"

#include "mongo/db/commands/find_and_modify.h"

#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/projection.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/operation_context_impl.h"
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
        /* this will eventually replace run,  once sort is handled */
        bool runNoDirectClient( OperationContext* txn, const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            verify( cmdObj["sort"].eoo() );

            const string ns = dbname + '.' + cmdObj.firstElement().valuestr();

            BSONObj query = cmdObj.getObjectField("query");
            BSONObj fields = cmdObj.getObjectField("fields");
            BSONObj update = cmdObj.getObjectField("update");
            
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

            bool ok = runNoDirectClient( txn, ns,
                                         query, fields, update,
                                         upsert, returnNew, remove,
                                         result, errmsg );

            if ( !ok && errmsg == "no-collection" ) {
                {
                    Lock::DBLock lk(txn->lockState(), dbname, MODE_X);
                    Client::Context ctx(txn, ns, false /* don't check version */);
                    Database* db = ctx.db();
                    if ( db->getCollection( txn, ns ) ) {
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
                }
                errmsg = "";
                ok = runNoDirectClient( txn, ns,
                                        query, fields, update,
                                        upsert, returnNew, remove,
                                        result, errmsg );
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

        static bool runNoDirectClient(OperationContext* txn,
                                      const string& ns, 
                                      const BSONObj& queryOriginal,
                                      const BSONObj& fields,
                                      const BSONObj& update,
                                      bool upsert,
                                      bool returnNew,
                                      bool remove ,
                                      BSONObjBuilder& result,
                                      string& errmsg) {

            Client::WriteContext cx(txn, ns);
            Collection* collection = cx.getCollection();

            const WhereCallbackReal whereCallback = WhereCallbackReal(txn, StringData(ns));

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
                massert(17383, "Could not canonicalize " + queryOriginal.toString(),
                    CanonicalQuery::canonicalize(ns, queryOriginal, &cq, whereCallback).isOK());

                PlanExecutor* rawExec;
                massert(17384, "Could not get plan executor for query " + queryOriginal.toString(),
                        getExecutor(txn,
                                    collection,
                                    cq,
                                    PlanExecutor::YIELD_AUTO,
                                    &rawExec,
                                    QueryPlannerParams::DEFAULT).isOK());

                scoped_ptr<PlanExecutor> exec(rawExec);

                PlanExecutor::ExecState state;
                if (PlanExecutor::ADVANCED == (state = exec->getNext(&doc, NULL))) {
                    found = true;
                }
            }

            WriteUnitOfWork wuow(txn);

            BSONObj queryModified = queryOriginal;
            if ( found && doc["_id"].type() && ! CanonicalQuery::isSimpleIdQuery( queryOriginal ) ) {
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

                BSONObjBuilder b( queryOriginal.objsize() + 10 );
                b.append( doc["_id"] );
                
                bool addedAtomic = false;

                BSONObjIterator i( queryOriginal );
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
                    deleteObjects(txn, cx.db(), ns, queryModified, true, true);
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
                    UpdateRequest request(txn, requestNs);

                    request.setQuery(queryModified);
                    request.setUpdates(update);
                    request.setUpsert(upsert);
                    request.setUpdateOpLog();
                    // TODO(greg) We need to send if we are ignoring
                    // the shard version below, but for now no
                    UpdateLifecycleImpl updateLifecycle(false, requestNs);
                    request.setLifecycle(&updateLifecycle);
                    UpdateResult res = mongo::update(cx.db(),
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
                                                   << " queryOriginal: " << queryOriginal;
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
            wuow.commit();
            return true;
        }
        
        virtual bool run(OperationContext* txn, const string& dbname, BSONObj& cmdObj, int x, string& errmsg, BSONObjBuilder& result, bool y) {
            DBDirectClient db(txn);

            if (cmdObj["sort"].eoo()) {
                return runNoDirectClient(txn, dbname, cmdObj, x, errmsg, result, y);
            }

            const string ns = dbname + '.' + cmdObj.firstElement().valuestr();

            BSONObj origQuery = cmdObj.getObjectField("query"); // defaults to {}
            Query q (origQuery);
            BSONElement sort = cmdObj["sort"];
            if (!sort.eoo())
                q.sort(sort.embeddedObjectUserCheck());

            bool upsert = cmdObj["upsert"].trueValue();

            BSONObj fieldsHolder (cmdObj.getObjectField("fields"));
            const BSONObj* fields = (fieldsHolder.isEmpty() ? NULL : &fieldsHolder);

            Projection projection;
            if (fields) {
                projection.init(fieldsHolder, WhereCallbackReal(txn, StringData(dbname)));
                if (!projection.includeID()) {
                    fields = NULL; // do projection in post-processing
                }
            }

            Lock::DBLock dbXLock(txn->lockState(), dbname, MODE_X);
            WriteUnitOfWork wunit(txn);
            Client::Context ctx(txn, ns);

            BSONObj out = db.findOne(ns, q, fields);
            if (out.isEmpty()) {
                if (!upsert) {
                    result.appendNull("value");
                    wunit.commit();
                    return true;
                }

                BSONElement update = cmdObj["update"];
                uassert(13329, "upsert mode requires update field", !update.eoo());
                uassert(13330, "upsert mode requires query field", !origQuery.isEmpty());
                db.update(ns, origQuery, update.embeddedObjectUserCheck(), true);

                BSONObj gle = db.getLastErrorDetailed(dbname);
                result.append("lastErrorObject", gle);
                if (gle["err"].type() == String) {
                    errmsg = gle["err"].String();
                    return false;
                }

                if (!cmdObj["new"].trueValue()) {
                    result.appendNull("value");
                    wunit.commit();
                    return true;
                }

                BSONObjBuilder bob;
                BSONElement _id = gle[kUpsertedFieldName];
                if (!_id.eoo())
                    bob.appendAs(_id, "_id");
                else
                    bob.appendAs(origQuery["_id"], "_id");

                out = db.findOne(ns, bob.done(), fields);

            }
            else {

                if (cmdObj["remove"].trueValue()) {
                    uassert(12515, "can't remove and update", cmdObj["update"].eoo());
                    db.remove(ns, QUERY("_id" << out["_id"]), 1);

                    BSONObj gle = db.getLastErrorDetailed(dbname);
                    result.append("lastErrorObject", gle);
                    if (gle["err"].type() == String) {
                        errmsg = gle["err"].String();
                        return false;
                    }

                }
                else {   // update

                    BSONElement queryId = origQuery["_id"];
                    if (queryId.eoo() || getGtLtOp(queryId) != BSONObj::Equality) {
                        // need to include original query for $ positional operator

                        BSONObjBuilder b;
                        b.append(out["_id"]);
                        BSONObjIterator it(origQuery);
                        while (it.more()) {
                            BSONElement e = it.next();
                            if (strcmp(e.fieldName(), "_id"))
                                b.append(e);
                        }
                        q = Query(b.obj());
                    }

                    if (q.isComplex()) // update doesn't work with complex queries
                        q = Query(q.getFilter().getOwned());

                    BSONElement update = cmdObj["update"];
                    uassert(12516, "must specify remove or update", !update.eoo());
                    db.update(ns, q, update.embeddedObjectUserCheck());

                    BSONObj gle = db.getLastErrorDetailed(dbname);
                    result.append("lastErrorObject", gle);
                    if (gle["err"].type() == String) {
                        errmsg = gle["err"].String();
                        return false;
                    }

                    if (cmdObj["new"].trueValue())
                        out = db.findOne(ns, QUERY("_id" << out["_id"]), fields);
                }
            }

            if (!fieldsHolder.isEmpty() && !fields){
                // we need to run projection but haven't yet
                out = projection.transform(out);
            }

            result.append("value", out);

            wunit.commit();
            return true;
        }
    } cmdFindAndModify;

}
