// query.h

#pragma once

#include "../stdafx.h"
#include "../grid/message.h"

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
      int nReturned;
      list of marshalled JSObjects;
*/ 

struct QueryResult : public MsgData {
	long long cursorId;
	int startingFrom;
	int nReturned;
	char data[4];
};

QueryResult* runQuery(const char *ns, const char *query, int ntoreturn);

