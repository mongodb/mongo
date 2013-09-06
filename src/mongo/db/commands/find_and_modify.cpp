// find_and_modify.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/commands/find_and_modify.h"

#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/instance.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/queryutil.h"

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
        virtual bool logTheOp() { return false; } // the modifications will be logged directly
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            find_and_modify::addPrivilegesRequiredForFindAndModify(dbname, cmdObj, out);
        }
        /* this will eventually replace run,  once sort is handled */
        bool runNoDirectClient( const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            verify( cmdObj["sort"].eoo() );

            string ns = dbname + '.' + cmdObj.firstElement().valuestr();

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
            else if ( update.isEmpty() ) {
                errmsg = "need remove or update";
                return false;
            }
            
            PageFaultRetryableSection s;
            while ( 1 ) {
                try {
                    return runNoDirectClient( ns , 
                                              query , fields , update , 
                                              upsert , returnNew , remove , 
                                              result , errmsg );
                }
                catch ( PageFaultException& e ) {
                    e.touch();
                }
            }

                    
        }

        void _appendHelper( BSONObjBuilder& result , const BSONObj& doc , bool found , const BSONObj& fields ) {
            if ( ! found ) {
                result.appendNull( "value" );
                return;
            }

            if ( fields.isEmpty() ) {
                result.append( "value" , doc );
                return;
            }

            Projection p;
            p.init( fields );
            result.append( "value" , p.transform( doc ) );
                
        }

        bool runNoDirectClient( const string& ns , 
                                const BSONObj& queryOriginal , const BSONObj& fields , const BSONObj& update , 
                                bool upsert , bool returnNew , bool remove ,
                                BSONObjBuilder& result , string& errmsg ) {
            
            
            Lock::DBWrite lk( ns );
            Client::Context cx( ns );

            BSONObj doc;
            
            bool found = Helpers::findOne( ns.c_str() , queryOriginal , doc );

            BSONObj queryModified = queryOriginal;
            if ( found && doc["_id"].type() && ! isSimpleIdQuery( queryOriginal ) ) {
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
                _appendHelper( result , doc , found , fields );
                if ( found ) {
                    deleteObjects( ns.c_str() , queryModified , true , true );
                    BSONObjBuilder le( result.subobjStart( "lastErrorObject" ) );
                    le.appendNumber( "n" , 1 );
                    le.done();
                }
            }
            else {
                // update
                if ( ! found && ! upsert ) {
                    // didn't have it, and am not upserting
                    _appendHelper( result , doc , found , fields );
                }
                else {
                    // we found it or we're updating
                    
                    if ( ! returnNew ) {
                        _appendHelper( result , doc , found , fields );
                    }
                    
                    const NamespaceString requestNs(ns);
                    UpdateRequest request(requestNs);

                    request.setQuery(queryModified);
                    request.setUpdates(update);
                    request.setUpsert(upsert);
                    request.setUpdateOpLog();

                    UpdateResult res = mongo::update(request, &cc().curop()->debug());

                    if ( returnNew ) {
                        if ( res.upserted.isSet() ) {
                            queryModified = BSON( "_id" << res.upserted );
                        }
                        else if ( queryModified["_id"].type() ) {
                            // we do this so that if the update changes the fields, it still matches
                            queryModified = queryModified["_id"].wrap();
                        }
                        if ( ! Helpers::findOne( ns.c_str() , queryModified , doc ) ) {
                            errmsg = str::stream() << "can't find object after modification  " 
                                                   << " ns: " << ns 
                                                   << " queryModified: " << queryModified 
                                                   << " queryOriginal: " << queryOriginal;
                            log() << errmsg << endl;
                            return false;
                        }
                        _appendHelper( result , doc , true , fields );
                    }
                    
                    BSONObjBuilder le( result.subobjStart( "lastErrorObject" ) );
                    le.appendBool( "updatedExisting" , res.existing );
                    le.appendNumber( "n" , res.numMatched );
                    if ( res.upserted.isSet() )
                        le.append( "upserted" , res.upserted );
                    le.done();
                    
                }
            }
            
            return true;
        }
        
        virtual bool run(const string& dbname, BSONObj& cmdObj, int x, string& errmsg, BSONObjBuilder& result, bool y) {
            static DBDirectClient db;

            if ( cmdObj["sort"].eoo() )
                return runNoDirectClient( dbname , cmdObj , x, errmsg , result, y );

            string ns = dbname + '.' + cmdObj.firstElement().valuestr();

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
                projection.init(fieldsHolder);
                if (!projection.includeID())
                    fields = NULL; // do projection in post-processing
            }

            BSONObj out = db.findOne(ns, q, fields);
            if (out.isEmpty()) {
                if (!upsert) {
                    result.appendNull("value");
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

                if (cmdObj["new"].trueValue()) {
                    BSONElement _id = gle["upserted"];
                    if (_id.eoo())
                        _id = origQuery["_id"];

                    out = db.findOne(ns, QUERY("_id" << _id), fields);
                }

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

            return true;
        }
    } cmdFindAndModify;


}
