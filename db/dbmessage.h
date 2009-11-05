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

#include "storage.h"
#include "jsobj.h"
#include "namespace.h"
#include "../util/message.h"

namespace mongo {

    /* db response format

       Query or GetMore: // see struct QueryResult
          int resultFlags;
          int64 cursorID;
          int startingFrom;
          int nReturned;
          list of marshalled JSObjects;
    */

    extern bool objcheck;
    
#pragma pack(1)
    struct QueryResult : public MsgData {
        enum ResultFlagType {
            ResultFlag_CursorNotFound = 1,   /* returned, with zero results, when getMore is called but the cursor id is not valid at the server. */
            ResultFlag_ErrSet = 2,           /* { $err : ... } is being returned */
            ResultFlag_ShardConfigStale = 4  /* have to update config from the server,  usually $err is also set */
        };

        long long cursorId;
        int startingFrom;
        int nReturned;
        const char *data() {
            return (char *) (((int *)&nReturned)+1);
        }
        int& resultFlags() {
            return dataAsInt();
        }
    };
#pragma pack()

    /* For the database/server protocol, these objects and functions encapsulate
       the various messages transmitted over the connection.
    */

    class DbMessage {
    public:
        DbMessage(const Message& _m) : m(_m) {
            theEnd = _m.data->_data + _m.data->dataLen();
            int *r = (int *) _m.data->_data;
            reserved = *r;
            r++;
            data = (const char *) r;
            nextjsobj = data;
        }

        const char * getns() {
            return data;
        }
        void getns(Namespace& ns) {
            ns = data;
        }
        
        
        void resetPull(){
            nextjsobj = data;
        }
        int pullInt() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            int i = *((int *)nextjsobj);
            nextjsobj += 4;
            return i;
        }
        long long pullInt64() const {
            return pullInt64();
        }
        long long &pullInt64() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            long long &i = *((long long *)nextjsobj);
            nextjsobj += 8;
            return i;
        }

        OID* getOID() {
            return (OID *) (data + strlen(data) + 1); // skip namespace
        }

        void getQueryStuff(const char *&query, int& ntoreturn) {
            int *i = (int *) (data + strlen(data) + 1);
            ntoreturn = *i;
            i++;
            query = (const char *) i;
        }

        /* for insert and update msgs */
        bool moreJSObjs() {
            return nextjsobj != 0;
        }
        BSONObj nextJsObj() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            massert( "Remaining data too small for BSON object", theEnd - nextjsobj > 3 );
            BSONObj js(nextjsobj);
            massert( "Invalid object size", js.objsize() > 3 );
            massert( "Next object larger than available space",
                    js.objsize() < ( theEnd - data ) );
            if ( objcheck && !js.valid() ) {
                massert("bad object in message", false);
            }            
            nextjsobj += js.objsize();
            if ( nextjsobj >= theEnd )
                nextjsobj = 0;
            return js;
        }

        const Message& msg() {
            return m;
        }

        void markSet(){
            mark = nextjsobj;
        }
        
        void markReset(){
            nextjsobj = mark;
        }

    private:
        const Message& m;
        int reserved;
        const char *data;
        const char *nextjsobj;
        const char *theEnd;

        const char * mark;
    };


    /* a request to run a query, received from the database */
    class QueryMessage {
    public:
        const char *ns;
        int ntoskip;
        int ntoreturn;
        int queryOptions;
        BSONObj query;
        auto_ptr< FieldMatcher > fields;
        
        /* parses the message into the above fields */
        QueryMessage(DbMessage& d) {
            ns = d.getns();
            ntoskip = d.pullInt();
            ntoreturn = d.pullInt();
            query = d.nextJsObj();
            if ( d.moreJSObjs() ) {
                BSONObj o = d.nextJsObj();
                if (!o.isEmpty()){
                    fields = auto_ptr< FieldMatcher >(new FieldMatcher() );
                    fields->add( o );
                }
            }
            queryOptions = d.msg().data->dataAsInt();
        }
    };

} // namespace mongo

#include "../client/dbclient.h"

namespace mongo {

    inline void replyToQuery(int queryResultFlags,
                             AbstractMessagingPort* p, Message& requestMsg,
                             void *data, int size,
                             int nReturned, int startingFrom = 0,
                             long long cursorId = 0
                            ) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        b.append(data, size);
        QueryResult *qr = (QueryResult *) b.buf();
        qr->resultFlags() = queryResultFlags;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->cursorId = cursorId;
        qr->startingFrom = startingFrom;
        qr->nReturned = nReturned;
        b.decouple();
        Message *resp = new Message();
        resp->setData(qr, true); // transport will free
        p->reply(requestMsg, *resp, requestMsg.data->id);
    }

} // namespace mongo

//#include "bsonobj.h"
#include "instance.h"

namespace mongo {

    /* object reply helper. */
    inline void replyToQuery(int queryResultFlags,
                             AbstractMessagingPort* p, Message& requestMsg,
                             BSONObj& responseObj)
    {
        replyToQuery(queryResultFlags,
                     p, requestMsg,
                     (void *) responseObj.objdata(), responseObj.objsize(), 1);
    }

    /* helper to do a reply using a DbResponse object */
    inline void replyToQuery(int queryResultFlags, Message &m, DbResponse &dbresponse, BSONObj obj) {
        BufBuilder b;
        b.skip(sizeof(QueryResult));
        b.append((void*) obj.objdata(), obj.objsize());
        QueryResult* msgdata = (QueryResult *) b.buf();
        b.decouple();
        QueryResult *qr = msgdata;
        qr->resultFlags() = queryResultFlags;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->cursorId = 0;
        qr->startingFrom = 0;
        qr->nReturned = 1;
        Message *resp = new Message();
        resp->setData(msgdata, true); // transport will free
        dbresponse.response = resp;
        dbresponse.responseTo = m.data->id;
    }

} // namespace mongo
