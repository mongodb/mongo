// @file distlock.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "dbclient.h"
#include "distlock.h"

namespace mongo {
       
    bool DistributedLock::lock_try( string why , BSONObj * other ){
        // recursive
        if ( getState() > 0 )
            return true;

        ScopedDbConnection conn( _conn );
            
        BSONObjBuilder queryBuilder;
        queryBuilder.appendElements( _id );
        queryBuilder.append( "state" , 0 );            

        { // make sure its there so we can use simple update logic below
            BSONObj o = conn->findOne( _ns , _id );
            if ( o.isEmpty() ){
                try {
                    conn->insert( _ns , BSON( "_id" << _name << "state" << 0 << "who" << "" ) );
                }
                catch ( UserException& ){
                }
            }
            else if ( o["state"].numberInt() > 0 ){
                return false;
            }
            else if ( o["ts"].type() ){
                queryBuilder.append( o["ts"] );
            }
        }
            
        OID ts;
        ts.init();

        bool gotLock = false;
        BSONObj now;
            
        BSONObj whatIWant = BSON( "$set" << BSON( "state" << 1 << "who" << myid() <<
                                                  "when" << DATENOW << "why" << why << "ts" << ts ) );
        try {
            conn->update( _ns , queryBuilder.obj() , whatIWant );
                
            BSONObj o = conn->getLastErrorDetailed();
            now = conn->findOne( _ns , _id );
                
            if ( o["n"].numberInt() == 0 ){
                if ( other )
                    *other = now;
                gotLock = false;
            }
            else {
                gotLock = true;
            }

        }
        catch ( UpdateNotTheSame& up ){
            // this means our update got through on some, but not others

            for ( unsigned i=0; i<up.size(); i++ ){
                ScopedDbConnection temp( up[i].first );
                BSONObj temp2 = temp->findOne( _ns , _id );

                if ( now.isEmpty() || now["ts"] < temp2["ts"] ){
                    now = temp2.getOwned();
                }

                temp.done();
            }

            if ( now["ts"].OID() == ts ){
                gotLock = true;
                conn->update( _ns , _id , whatIWant );
            }
            else {
                gotLock = false;
            }
        }
            
        conn.done();
            
        log(1) << "dist_lock lock gotLock: " << gotLock << " now: " << now << endl;

        if ( ! gotLock )
            return false;
            
        _state.set( 1 );
        return true;
    }

    void DistributedLock::unlock(){
        ScopedDbConnection conn( _conn );
        conn->update( _ns , _id, BSON( "$set" << BSON( "state" << 0 ) ) );
        log(1) << "dist_lock unlock: " << conn->findOne( _ns , _id ) << endl;
        conn.done();
            
        _state.set( 0 );
    }


}
