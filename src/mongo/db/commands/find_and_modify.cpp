// find_and_modify.cpp

/**
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
*/

#include "pch.h"
#include "../commands.h"
#include "../instance.h"
#include "../clientcursor.h"
#include "../pagefault.h"
#include "../dbhelpers.h"
#include "../ops/delete.h"
#include "../ops/update.h"

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
                                              result );
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
                                const BSONObj& query , const BSONObj& fields , const BSONObj& update , 
                                bool upsert , bool returnNew , bool remove ,
                                BSONObjBuilder& result ) {
            
            
            Lock::DBWrite lk( ns );
            
            BSONObj doc;
            
            bool found = Helpers::findOne( ns.c_str() , query , doc );
            if ( remove ) {
                _appendHelper( result , doc , found , fields );
                if ( found ) {
                    deleteObjects( ns.c_str() , query , true , true );
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
                    
                    updateObjects( ns.c_str() , update , query , upsert , false , true , cc().curop()->debug() );
                    
                    if ( returnNew ) {
                        verify( Helpers::findOne( ns.c_str() , query , doc ) );
                        _appendHelper( result , doc , true , fields );
                    }
                    
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

                BSONObj gle = db.getLastErrorDetailed();
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

                    BSONObj gle = db.getLastErrorDetailed();
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

                    BSONObj gle = db.getLastErrorDetailed();
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
