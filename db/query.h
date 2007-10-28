// query.h

#pragma once

#include "../stdafx.h"
#include "../grid/message.h"
#include "jsobj.h"

/* requests:

   Query:
      int reserved;
      string collection;
	  int nToReturn; // how many you want back as the beginning of the cursor data
      JSObject query;
   GetMore:
      int reserved;;
      int64 cursorID;
      int nToReturn;
*/

/* db response format

   Query or GetMore:
      int reserved;
      int64 cursorID;
      int startingFrom;
      int nReturned; // 0=infinity
      list of marshalled JSObjects;
*/

struct QueryResult : public MsgData {
	long long cursorId;
	int startingFrom;
	int nReturned;
	const char *data() { return (char *) (((int *)&nReturned)+1); }
};

QueryResult* runQuery(const char *ns, int ntoreturn, JSObj);

