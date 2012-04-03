// dbmessage.cpp

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

#include "pch.h"
#include "dbmessage.h"

namespace mongo {

    string Message::toString() const {
        stringstream ss;
        ss << "op: " << opToString( operation() ) << " len: " << size();
        if ( operation() >= 2000 && operation() < 2100 ) {
            DbMessage d(*this);
            ss << " ns: " << d.getns();
            switch ( operation() ) {
            case dbUpdate: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                BSONObj o = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q << " update: " << o;
                break;
            }
            case dbInsert:
                ss << d.nextJsObj();
                break;
            case dbDelete: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q;
                break;
            }
            default:
                ss << " CANNOT HANDLE YET";
            }


        }
        return ss.str();
    }


    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      void *data, int size,
                      int nReturned, int startingFrom,
                      long long cursorId 
                      ) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        b.appendBuf(data, size);
        QueryResult *qr = (QueryResult *) b.buf();
        qr->_resultFlags() = queryResultFlags;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->cursorId = cursorId;
        qr->startingFrom = startingFrom;
        qr->nReturned = nReturned;
        b.decouple();
        Message resp(qr, true);
        p->reply(requestMsg, resp, requestMsg.header()->id);
    }

    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      BSONObj& responseObj) {
        replyToQuery(queryResultFlags,
                     p, requestMsg,
                     (void *) responseObj.objdata(), responseObj.objsize(), 1);
    }

    void replyToQuery(int queryResultFlags, Message &m, DbResponse &dbresponse, BSONObj obj) {
        BufBuilder b;
        b.skip(sizeof(QueryResult));
        b.appendBuf((void*) obj.objdata(), obj.objsize());
        QueryResult* msgdata = (QueryResult *) b.buf();
        b.decouple();
        QueryResult *qr = msgdata;
        qr->_resultFlags() = queryResultFlags;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->cursorId = 0;
        qr->startingFrom = 0;
        qr->nReturned = 1;
        Message *resp = new Message();
        resp->setData(msgdata, true); // transport will free
        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
    }



}
