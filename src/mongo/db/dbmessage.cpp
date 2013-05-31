// dbmessage.cpp

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

#include "mongo/pch.h"

#include "mongo/db/dbmessage.h"

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
                      const BSONObj& responseObj) {
        replyToQuery(queryResultFlags,
                     p, requestMsg,
                     (void *) responseObj.objdata(), responseObj.objsize(), 1);
    }

    void replyToQuery( int queryResultFlags, Message &m, DbResponse &dbresponse, BSONObj obj ) {
        Message *resp = new Message();
        replyToQuery( queryResultFlags, *resp, obj );
        dbresponse.response = resp;
        dbresponse.responseTo = m.header()->id;
    }

    void replyToQuery( int queryResultFlags, Message& response, const BSONObj& resultObj ) {
        BufBuilder bufBuilder;
        bufBuilder.skip( sizeof( QueryResult ));
        bufBuilder.appendBuf( reinterpret_cast< void *>(
                const_cast< char* >( resultObj.objdata() )), resultObj.objsize() );

        QueryResult* queryResult = reinterpret_cast< QueryResult* >( bufBuilder.buf() );
        bufBuilder.decouple();

        queryResult->_resultFlags() = queryResultFlags;
        queryResult->len = bufBuilder.len();
        queryResult->setOperation( opReply );
        queryResult->cursorId = 0;
        queryResult->startingFrom = 0;
        queryResult->nReturned = 1;

        response.setData( queryResult, true ); // transport will free
    }

}
