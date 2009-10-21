// query.h

/**
*    Copyright (C) 2008 10gen Inc.
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

#pragma once

#include "../stdafx.h"
#include "../util/message.h"
#include "dbmessage.h"
#include "jsobj.h"
#include "storage.h"

/* db request message format

   unsigned opid;         // arbitary; will be echoed back
   byte operation;
   int options;

   then for:

   dbInsert:
      string collection;
      a series of JSObjects
   dbDelete:
      string collection;
	  int flags=0; // 1=DeleteSingle
      JSObject query;
   dbUpdate:
      string collection;
	  int flags; // 1=upsert
      JSObject query;
	  JSObject objectToUpdate;
        objectToUpdate may include { $inc: <field> } or { $set: ... }, see struct Mod.
   dbQuery:
      string collection;
	  int nToSkip;
	  int nToReturn; // how many you want back as the beginning of the cursor data (0=no limit)            
                     // greater than zero is simply a hint on how many objects to send back per "cursor batch".
                     // a negative number indicates a hard limit.
      JSObject query;
	  [JSObject fieldsToReturn]
   dbGetMore:
	  string collection; // redundant, might use for security.
      int nToReturn;
      int64 cursorID;
   dbKillCursors=2007:
      int n;
	  int64 cursorIDs[n];

   Note that on Update, there is only one object, which is different
   from insert where you can pass a list of objects to insert in the db.
   Note that the update field layout is very similar layout to Query.
*/

// struct QueryOptions, QueryResult, QueryResultFlags in:
#include "../client/dbclient.h"

namespace mongo {

    // for an existing query (ie a ClientCursor), send back additional information.
    QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid , stringstream& ss);

    struct UpdateResult {
        bool existing;
        bool mod;
        unsigned long long num;

        UpdateResult( bool e, bool m, unsigned long long n )
            : existing(e) , mod(m), num(n ){}

        int oldCode(){
            if ( ! num )
                return 0;
            
            if ( existing ){
                if ( mod )
                    return 2;
                return 1;
            }
            
            if ( mod )
                return 3;
            return 4;
        }
    };
    
    /* returns true if an existing object was updated, false if no existing object was found.
       multi - update multiple objects - mostly useful with things like $set
    */
    UpdateResult updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, bool multi , stringstream& ss, bool logop );

    // If justOne is true, deletedId is set to the id of the deleted object.
    int deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool logop = false, bool god=false);

    long long runCount(const char *ns, const BSONObj& cmd, string& err);
    
    auto_ptr< QueryResult > runQuery(Message& m, stringstream& ss );
    
} // namespace mongo

#include "clientcursor.h"
