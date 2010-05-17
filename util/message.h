// message.h

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

#pragma once

#include "../util/sock.h"
#include "../bson/util/atomic_int.h"
#include "hostandport.h"

namespace mongo {

    extern bool noUnixSocket;

    class Message;
    class MessagingPort;
    class PiggyBackData;
    typedef AtomicUInt MSGID;

    class Listener {
    public:
        Listener(const string &ip, int p, bool logConnect=true ) : _port(p), _ip(ip), _logConnect(logConnect) { }
        virtual ~Listener() {}
        void initAndListen(); // never returns unless error (start a thread)

        /* spawn a thread, etc., then return */
        virtual void accepted(int sock, const SockAddr& from);
        virtual void accepted(MessagingPort *mp){
            assert(!"You must overwrite one of the accepted methods");
        }

        const int _port;

    private:
        string _ip;
        bool _logConnect;
    };

    class AbstractMessagingPort {
    public:
        virtual ~AbstractMessagingPort() { }
        virtual void reply(Message& received, Message& response, MSGID responseTo) = 0; // like the reply below, but doesn't rely on received.data still being available
        virtual void reply(Message& received, Message& response) = 0;
        
        virtual HostAndPort remote() const = 0;
        virtual unsigned remotePort() const = 0;
    };

    class MessagingPort : public AbstractMessagingPort {
    public:
        MessagingPort(int sock, const SockAddr& farEnd);

        // in some cases the timeout will actually be 2x this value - eg we do a partial send,
        // then the timeout fires, then we try to send again, then the timeout fires again with
        // no data sent, then we detect that the other side is down
        MessagingPort(int timeout = 0, int logLevel = 0 );

        virtual ~MessagingPort();

        void shutdown();
        
        bool connect(SockAddr& farEnd);

        /* it's assumed if you reuse a message object, that it doesn't cross MessagingPort's.
           also, the Message data will go out of scope on the subsequent recv call.
        */
        bool recv(Message& m);
        void reply(Message& received, Message& response, MSGID responseTo);
        void reply(Message& received, Message& response);
        bool call(Message& toSend, Message& response);
        void say(Message& toSend, int responseTo = -1);

        void piggyBack( Message& toSend , int responseTo = -1 );

        virtual unsigned remotePort() const;
        virtual HostAndPort remote() const;

        // send len or throw SocketException
        void send( const char * data , int len, const char *context ) {
            vector< pair< char *, int > > temp;
            temp.push_back( make_pair( const_cast< char * >( data ), len ) );
            send( temp, context );
        }
        void send( const vector< pair< char *, int > > &data, const char *context );

        // recv len or throw SocketException
        void recv( char * data , int len );
        
        int unsafe_recv( char *buf, int max );
    private:
        int sock;
        PiggyBackData * piggyBackData;
    public:
        SockAddr farEnd;
        int _timeout;
        int _logLevel; // passed to log() when logging errors

        friend class PiggyBackData;
    };

    //#pragma pack()
#pragma pack(1)

    enum Operations {
        opReply = 1,     /* reply. responseTo is set. */
        dbMsg = 1000,    /* generic msg command followed by a string */
        dbUpdate = 2001, /* update object */
        dbInsert = 2002,
        //dbGetByOID = 2003,
        dbQuery = 2004,
        dbGetMore = 2005,
        dbDelete = 2006,
        dbKillCursors = 2007
    };

    bool doesOpGetAResponse( int op );

    inline const char * opToString( int op ){
        switch ( op ){
        case 0: return "none";
        case opReply: return "reply";
        case dbMsg: return "msg";
        case dbUpdate: return "update";
        case dbInsert: return "insert";
        case dbQuery: return "query";
        case dbGetMore: return "getmore";
        case dbDelete: return "remove";
        case dbKillCursors: return "killcursors";
        default: 
            PRINT(op);
            assert(0); 
            return "";
        }
    }

    struct MsgData {
        int len; /* len of the msg, including this field */
        MSGID id; /* request/reply id's match... */
        MSGID responseTo; /* id of the message we are responding to */
        int _operation;
        int operation() const {
            return _operation;
        }
        void setOperation(int o) {
            _operation = o;
        }
        char _data[4];

        int& dataAsInt() {
            return *((int *) _data);
        }
        
        bool valid(){
            if ( len <= 0 || len > ( 1024 * 1024 * 10 ) )
                return false;
            if ( _operation < 0 || _operation > 100000 )
                return false;
            return true;
        }

        int dataLen(); // len without header
    };
    const int MsgDataHeaderSize = sizeof(MsgData) - 4;
    inline int MsgData::dataLen() {
        return len - MsgDataHeaderSize;
    }

#pragma pack()

    class Message {
    public:
        Message() {
            _freeIt = false;
        }
        Message( void * data , bool freeIt ) {
            _setData( reinterpret_cast< MsgData* >( data ), freeIt );
        };
        ~Message() {
            reset();
        }
        
        SockAddr _from;

        MsgData *header() const { return reinterpret_cast< MsgData* > ( _data[ 0 ].first ); }
        int operation() const { return header()->operation(); }
        
        MsgData *singleData() const {
            massert( 13273, "single data buffer expected", _data.size() == 1 );
            return header();
        }

        const vector< pair< char *, int > > &allData() const { return _data; }
        
        bool empty() const { return _data.empty(); }
        
        // concat multiple buffers - noop if <2 buffers already, otherwise can be expensive copy
        // can get rid of this if we make response handling smarter
        void concat() {
            if ( _data.size() < 2 ) {
                return;
            }
            
            assert( _freeIt );
            int totalSize = 0;
            for( vector< pair< char *, int > >::const_iterator i = _data.begin(); i != _data.end(); ++i ) {
                totalSize += i->second;
            }
            char *buf = (char*)malloc( totalSize );
            char *p = buf;
            for( vector< pair< char *, int > >::const_iterator i = _data.begin(); i != _data.end(); ++i ) {
                memcpy( p, i->first, i->second );
                p += i->second;
            }
            reset();
            setData( reinterpret_cast< MsgData* >( buf ), true );
        }
        
        Message& operator=(Message& r) {
            assert( _data.empty() );
            assert( r._freeIt );
            _data = r._data;
            r._freeIt = false;
            r._data.clear();
            _freeIt = true;
            return *this;
        }

        void reset() {
            if ( _freeIt ) {
                for( vector< pair< char *, int > >::const_iterator i = _data.begin(); i != _data.end(); ++i ) {
                    free(i->first);
                }
            }
            _data.clear();
            _freeIt = false;
        }

        void setData(MsgData *d, bool freeIt) {
            assert( _data.empty() );
            _setData( d, freeIt );
        }
        void setData(int operation, const char *msgtxt) {
            setData(operation, msgtxt, strlen(msgtxt)+1);
        }
        void setData(int operation, const char *msgdata, size_t len) {
            assert( _data.empty() );
            size_t dataLen = len + sizeof(MsgData) - 4;
            MsgData *d = (MsgData *) malloc(dataLen);
            memcpy(d->_data, msgdata, len);
            d->len = fixEndian(dataLen);
            d->setOperation(operation);
            _setData( d, true );
        }

        bool doIFreeIt() {
            return _freeIt;
        }

    private:
        void _setData( MsgData *d, bool freeIt ) {
            _freeIt = freeIt;
            _data.push_back( make_pair( reinterpret_cast< char * >( d ), d->len ) );
        }
        // byte buffer(s) - the first must contain at least a full MsgData
        vector< pair< char*, int > > _data;
        bool _freeIt;
    };

    class SocketException : public DBException {
    public:
        virtual const char* what() const throw() { return "socket exception"; }
        virtual int getCode() const { return 9001; }
    };

    MSGID nextMessageId();

    void setClientId( int id );
    int getClientId();

    extern TicketHolder connTicketHolder;

} // namespace mongo
