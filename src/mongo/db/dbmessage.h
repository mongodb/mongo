// dbmessage.h

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

#include "jsobj.h"
#include "../bson/util/bswap.h"
#include "namespace-inl.h"
#include "../util/net/message.h"
#include "../client/constants.h"
#include "instance.h"

namespace mongo {

    /* db response format

       Query or GetMore: // see struct QueryResult
          int resultFlags;
          int64 cursorID;
          int startingFrom;
          int nReturned;
          list of marshalled JSObjects;
    */

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


#pragma pack(1)
    struct QueryResult : public MsgData {
        little<long long> cursorId;
        little<int> startingFrom;
        little<int> nReturned;
        const char *data() {
            return reinterpret_cast<char*>( &nReturned ) + 4;
        }
        int resultFlags() {
            return dataAsInt();
        }
        little<int>& _resultFlags() {
            return dataAsInt();
        }
        void setResultFlagsToOk() {
            _resultFlags() = ResultFlag_AwaitCapable;
        }
        void initializeResultFlags() {
            _resultFlags() = 0;   
        }
    };

#pragma pack()

    /* For the database/server protocol, these objects and functions encapsulate
       the various messages transmitted over the connection.

       See http://www.mongodb.org/display/DOCS/Mongo+Wire+Protocol
    */
    class DbMessage {
    public:
        DbMessage(const Message& _m) : m(_m) , mark(0) {
            // for received messages, Message has only one buffer
            theEnd = _m.singleData()->_data + _m.header()->dataLen();
            char *r = _m.singleData()->_data;
            reserved = &little<int>::ref( r );
            data = r + 4;
            nextjsobj = data;
        }

        /** the 32 bit field before the ns 
         * track all bit usage here as its cross op
         * 0: InsertOption_ContinueOnError
         * 1: fromWriteback
         */
        little<int>& reservedField() { return *reserved; }

        const char * getns() const {
            return data;
        }
        void getns(Namespace& ns) const {
            ns = data;
        }

        const char * afterNS() const {
            return data + strlen( data ) + 1;
        }

        int getInt( int num ) const {
            const little<int>* foo = &little<int>::ref( afterNS() );
            return foo[num];
        }

        int getQueryNToReturn() const {
            return getInt( 1 );
        }

        /**
         * get an int64 at specified offsetBytes after ns
         */
        long long getInt64( int offsetBytes ) const {
            return little<long long>::ref( afterNS() + offsetBytes );
        }

        void resetPull() { nextjsobj = data; }
        int pullInt() const { return pullInt(); }

        const little<int>& pullInt() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            const little<int>& i = little<int>::ref( nextjsobj );
            nextjsobj += 4;
            return i;
        }

        little<long long>& pullInt64() {
            if ( nextjsobj == data )
                nextjsobj += strlen(data) + 1; // skip namespace
            little<long long>& i = little<long long>::ref( const_cast<char*>( nextjsobj ) );
            nextjsobj += 8;
            return i;
        }

        OID* getOID() const {
            return (OID *) (data + strlen(data) + 1); // skip namespace
        }

        void getQueryStuff(const char *&query, int& ntoreturn) {
            const char* tmp = data + strlen(data) + 1;
            ntoreturn = little<int>::ref( tmp );
            query = tmp + 4;
        }

        /* for insert and update msgs */
        bool moreJSObjs() const {
            return nextjsobj != 0;
        }
        BSONObj nextJsObj() {
            if ( nextjsobj == data ) {
                nextjsobj += strlen(data) + 1; // skip namespace
                massert( 13066 ,  "Message contains no documents", theEnd > nextjsobj );
            }
            massert( 10304 ,  "Client Error: Remaining data too small for BSON object", theEnd - nextjsobj > 3 );
            BSONObj js(nextjsobj);
            massert( 10305 ,  "Client Error: Invalid object size", js.objsize() > 3 );
            massert( 10306 ,  "Client Error: Next object larger than space left in message",
                     js.objsize() < ( theEnd - data ) );
            if ( cmdLine.objcheck && !js.valid() ) {
                massert( 10307 , "Client Error: bad object in message", false);
            }
            nextjsobj += js.objsize();
            if ( nextjsobj >= theEnd )
                nextjsobj = 0;
            return js;
        }

        const Message& msg() const { return m; }

        void markSet() {
            mark = nextjsobj;
        }

        void markReset() {
            verify( mark );
            nextjsobj = mark;
        }

    private:
        const Message& m;
        little<int>* reserved;
        const char *data;
        const char *nextjsobj;
        const char *theEnd;

        const char * mark;

    public:
        enum ReservedOptions {
            Reserved_InsertOption_ContinueOnError = 1 << 0 , 
            Reserved_FromWriteback = 1 << 1 
        };
    };


    /* a request to run a query, received from the database */
    class QueryMessage {
    public:
        const char *ns;
        int ntoskip;
        int ntoreturn;
        int queryOptions;
        BSONObj query;
        BSONObj fields;

        /* parses the message into the above fields */
        QueryMessage(DbMessage& d) {
            ns = d.getns();
            ntoskip = d.pullInt();
            ntoreturn = d.pullInt();
            query = d.nextJsObj();
            if ( d.moreJSObjs() ) {
                fields = d.nextJsObj();
            }
            queryOptions = d.msg().header()->dataAsInt();
        }
    };

    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      void *data, int size,
                      int nReturned, int startingFrom = 0,
                      long long cursorId = 0
                      );


    /* object reply helper. */
    void replyToQuery(int queryResultFlags,
                      AbstractMessagingPort* p, Message& requestMsg,
                      BSONObj& responseObj);

    /* helper to do a reply using a DbResponse object */
    void replyToQuery(int queryResultFlags, Message &m, DbResponse &dbresponse, BSONObj obj);


} // namespace mongo
