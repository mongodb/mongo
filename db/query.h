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

/* requests:

   dbDelete
      int reserved=0;
      string collection;
	  int flags=0; // 1=DeleteSingle
      JSObject query;
   dbUpdate:
      int reserved;
      string collection;
	  int flags; // 1=upsert
      JSObject query;
	  JSObject objectToUpdate;
        objectToUpdate may include { $inc: <field> }.
   dbQuery:
      int reserved;
      string collection;
	  int nToSkip;
	  int nToReturn; // how many you want back as the beginning of the cursor data
      JSObject query;
	  [JSObject fieldsToReturn]
   dbGetMore:
      int reserved;
	  string collection; // redundant, might use for security.
      int nToReturn;
      int64 cursorID;
   dbKillCursors=2007:
      int reserved;
      int n;
	  int64 cursorIDs[n];

   Note that on Update, there is only one object, which is different
   from insert where you can pass a list of objects to insert in the db.
   Note that the update field layout is very similar layout to Query.
*/

/* db response format

   Query or GetMore:
      int reserved;
      int64 cursorID;
      int startingFrom;
      int nReturned; // 0=infinity
      list of marshalled JSObjects;
*/

#pragma pack(push)
#pragma pack(1)

struct QueryResult : public MsgData {
	long long cursorId;
	int startingFrom;
	int nReturned;
	const char *data() { return (char *) (((int *)&nReturned)+1); }
};

#pragma pack(pop)

QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid);

// caller must free() returned QueryResult.
QueryResult* runQuery(Message&, const char *ns, int ntoskip, int ntoreturn, 
					  JSObj j, auto_ptr< set<string> > fieldFilter,
					  stringstream&);

void updateObjects(const char *ns, JSObj updateobj, JSObj pattern, bool upsert, stringstream& ss);
int deleteObjects(const char *ns, JSObj pattern, bool justOne, bool god=false);

#include "clientcursor.h"
