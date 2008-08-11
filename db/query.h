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
#include "../grid/message.h"
#include "jsobj.h"
#include "storage.h"

/* db request message format 

   unsigned opid;         // arbitary; will be echoed back
   byte operation;
   int options;

   then for:

   dbInsert:
      string collection;
      a series of JSObjects terminated with a null object (i.e., just EOO)
   dbDelete:
      string collection;
	  int flags=0; // 1=DeleteSingle
      JSObject query;
   dbUpdate:
      string collection;
	  int flags; // 1=upsert
      JSObject query;
	  JSObject objectToUpdate;
        objectToUpdate may include { $inc: <field> }.
   dbQuery:
      string collection;
	  int nToSkip;
	  int nToReturn; // how many you want back as the beginning of the cursor data
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

/* the field 'options' above can have these bits set: */
enum { 
    /* Sticky means cursor is not closed when the last data is retrieved.  rather, the cursor "sticks"
       on the final object's position.  you can resume using the cursor later, from where it was located, 
       if more data were received.  Set on dbQuery and dbGetMore.

       like any "latent cursor", the cursor may become invalid at some point -- for example if that 
       final object it references were deleted.  Thus, you should be prepared to requery if you get back 
       ResultOption_CursorNotFound.
    */
    Option_CursorSticky = 2
};

/* db response format

   Query or GetMore: // see struct QueryResult
      int resultOptions = 0;
      int64 cursorID;
      int startingFrom;
      int nReturned; // 0=infinity
      list of marshalled JSObjects;
*/

/* the field 'resultOptions' above */
enum { 
    /* returned, with zero results, when getMore is called but the cursor id is not valid at the server. */
    ResultOption_CursorNotFound = 1
};

// grab struct QueryResult from:
#include "dbclient.h"

// for an existing query (ie a ClientCursor), send back additional information.
QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid);

// caller must free() returned QueryResult.
QueryResult* runQuery(Message&, const char *ns, int ntoskip, int ntoreturn, 
					  JSObj j, auto_ptr< set<string> > fieldFilter,
					  stringstream&);

void updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert, stringstream& ss);

int deleteObjects(const char *ns, JSObj pattern, bool justOne, bool god=false);

#include "clientcursor.h"
