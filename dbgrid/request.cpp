// dbgrid/request.cpp

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

#include "stdafx.h"
#include "../grid/message.h"
#include "../db/dbmessage.h"
#include "connpool.h"

void writeOp(int op, Message& m, MessagingPort& p) {
  DbMessage d(m);
  const char *ns = d.getns();



  while( d.moreJSObjs() ) {
    JSObj js = d.nextJsObj();
    const char *ns = d.getns();
    assert(*ns);
    //
    //		setClient(ns);
    //		ss << ns;
    //		theDataFileMgr.insert(ns, (void*) js.objdata(), js.objsize());
    //		logOp("i", ns, js);
  }
}

void processRequest(Message& m, MessagingPort& p) {
    int op = m.data->operation();
    writeOp(op, m, p);
}
