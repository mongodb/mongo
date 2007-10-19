// query.h

#pragma once

#include "../stdafx.h"
#include "../grid/message.h"

/*
   Query or GetMore:
      int reserved;
      unsigned cursorID;
      unsigned startOfs;
      unsigned nReturned;
      list of marshalled JSObjects;
*/ 

struct QueryResult : public MsgData {
	int cursorId;
	int startOfs;
	int nReturned;
	char data[4];
};

QueryResult* runQuery(const char *ns, const char *query, int ntoreturn);

