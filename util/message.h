// Message.h

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

    class Listener : boost::noncopyable {
    public:
        Listener(const string &ip, int p, bool logConnect=true ) : _port(p), _ip(ip), _logConnect(logConnect), _elapsedTime(0){ }
        virtual ~Listener() {
            if ( _timeTracker == this )
                _timeTracker = 0;
        }
        void initAndListen(); // never returns unless error (start a thread)

        /* spawn a thread, etc., then return */
        virtual void accepted(int sock, const SockAddr& from);
        virtual void accepted(MessagingPort *mp){
            assert(!"You must overwrite one of the accepted methods");
        }

        const int _port;
        
        /**
         * @return a rough estimate of elepased time since the server started
         */
        long long getMyElapsedTimeMillis() const { return _elapsedTime; }

        void setAsTimeTracker(){
            _timeTracker = this;
        }

        static const Listener* getTimeTracker(){
            return _timeTracker;
        }
        
        static long long getElapsedTimeMillis() { 
            if ( _timeTracker )
                return _timeTracker->getMyElapsedTimeMillis();
            return 0;
        }

    private:
        string _ip;
        bool _logConnect;
        long long _elapsedTime;

        static const Listener* _timeTracker;
    };

    class AbstractMessagingPort : boost::noncopyable {
    public:
        virtual ~AbstractMessagingPort() { }
        virtual void reply(Message& received, Message& response, MSGID responseTo) = 0; // like the reply below, but doesn't rely on received.data still being available
        virtual void reply(Message& received, Message& response) = 0;
        
        virtual HostAndPort remote() const = 0;
        virtual unsigned remotePort() const = 0;

        virtual int getClientId(){
            int x = remotePort();
            x = x << 16;
            return x;
        }
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
        void send( const char * data , int len, const char *context );
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

        /* ports can be tagged with various classes.  see closeAllSockets(tag). defaults to 0. */
        unsigned tag;

        friend class PiggyBackData;
    };

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
    
    inline bool opIsWrite( int op ){
        switch ( op ){

        case 0: 
        case opReply: 
        case dbMsg: 
        case dbQuery: 
        case dbGetMore: 
        case dbKillCursors: 
            return false;
            
        case dbUpdate: 
        case dbInsert: 
        case dbDelete: 
            return false;

        default: 
            PRINT(op);
            assert(0); 
            return "";
        }
        
    }

#pragma pack(1)
/* see http://www.mongodb.org/display/DOCS/Mongo+Wire+Protocol 
*/
struct MSGHEADER { 
    int messageLength; // total message size, including this
    int requestID;     // identifier for this message
    int responseTo;    // requestID from the original request
                       //   (used in reponses from db)
    int opCode;     
};
struct OP_GETMORE : public MSGHEADER {
    MSGHEADER header;             // standard message header
    int       ZERO_or_flags;      // 0 - reserved for future use
    //cstring   fullCollectionName; // "dbname.collectionname"
    //int32     numberToReturn;     // number of documents to return
    //int64     cursorID;           // cursorID from the OP_REPLY
};
#pragma pack()

#pragma pack(1)
    /* todo merge this with MSGHEADER (or inherit from it). */
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

        long long getCursor(){
            assert( responseTo > 0 );
            assert( _operation == opReply );
            long long * l = (long long *)(_data + 4);
            return l[0];
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
        // we assume here that a vector with initial size 0 does no allocation (0 is the default, but wanted to make it explicit).
        Message() : _buf( 0 ), _data( 0 ), _freeIt( false ) {}
        Message( void * data , bool freeIt ) :
            _buf( 0 ), _data( 0 ), _freeIt( false ) {
            _setData( reinterpret_cast< MsgData* >( data ), freeIt );
        };
        Message(Message& r) : _buf( 0 ), _data( 0 ), _freeIt( false ) { 
            *this = r;
        }
        ~Message() {
            reset();
        }
        
        SockAddr _from;

        MsgData *header() const {
            assert( !empty() );
            return _buf ? _buf : reinterpret_cast< MsgData* > ( _data[ 0 ].first );
        }
        int operation() const { return header()->operation(); }
        
        MsgData *singleData() const {
            massert( 13273, "single data buffer expected", _buf );
            return header();
        }

        bool empty() const { return !_buf && _data.empty(); }

        int size() const{
            int res = 0;
            if ( _buf ){
                res =  _buf->len;
            } else {
                for (MsgVec::const_iterator it = _data.begin(); it != _data.end(); ++it){
                    res += it->second;
                }
            }
            return res;
        }
        
        // concat multiple buffers - noop if <2 buffers already, otherwise can be expensive copy
        // can get rid of this if we make response handling smarter
        void concat() {
            if ( _buf || empty() ) {
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
            _setData( (MsgData*)buf, true );
        }
        
        // vector swap() so this is fast
        Message& operator=(Message& r) {
            assert( empty() );
            assert( r._freeIt );
            _buf = r._buf;
            r._buf = 0;
            if ( r._data.size() > 0 ) {
                _data.swap( r._data );
            }
            r._freeIt = false;
            _freeIt = true;
            return *this;
        }

        void reset() {
            if ( _freeIt ) {
                if ( _buf ) {
                    free( _buf );
                }
                for( vector< pair< char *, int > >::const_iterator i = _data.begin(); i != _data.end(); ++i ) {
                    free(i->first);
                }
            }
            _buf = 0;
            _data.clear();
            _freeIt = false;
        }

        // use to add a buffer
        // assumes message will free everything
        void appendData(char *d, int size) {
            if ( size <= 0 ) {
                return;
            }
            if ( empty() ) {
                MsgData *md = (MsgData*)d;
                md->len = size; // can be updated later if more buffers added
                _setData( md, true );
                return;
            }
            assert( _freeIt );
            if ( _buf ) {
                _data.push_back( make_pair( (char*)_buf, _buf->len ) );
                _buf = 0;
            }
            _data.push_back( make_pair( d, size ) );
            header()->len += size;
        }
        
        // use to set first buffer if empty
        void setData(MsgData *d, bool freeIt) {
            assert( empty() );
            _setData( d, freeIt );
        }
        void setData(int operation, const char *msgtxt) {
            setData(operation, msgtxt, strlen(msgtxt)+1);
        }
        void setData(int operation, const char *msgdata, size_t len) {
            assert( empty() );
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

        void send( MessagingPort &p, const char *context ) {
            if ( empty() ) {
                return;
            }
            if ( _buf != 0 ) {
                p.send( (char*)_buf, _buf->len, context );
            } else {
                p.send( _data, context );
            }
        }

    private:
        void _setData( MsgData *d, bool freeIt ) {
            _freeIt = freeIt;
            _buf = d;
        }
        // if just one buffer, keep it in _buf, otherwise keep a sequence of buffers in _data
        MsgData * _buf;
        // byte buffer(s) - the first must contain at least a full MsgData unless using _buf for storage instead
        typedef vector< pair< char*, int > > MsgVec;
        MsgVec _data;
        bool _freeIt;
    };

    class SocketException : public DBException {
    public:
        enum Type { CLOSED , RECV_ERROR , SEND_ERROR } type;
        SocketException( Type t ) : DBException( "socket exception" , 9001 ) , type(t){}
        
        bool shouldPrint() const {
            return type != CLOSED;
        }
        
    };

    MSGID nextMessageId();

    void setClientId( int id );
    int getClientId();

    extern TicketHolder connTicketHolder;

    class ElapsedTracker {
    public:
        ElapsedTracker( int hitsBetweenMarks , int msBetweenMarks )
            : _h( hitsBetweenMarks ) , _ms( msBetweenMarks ) , _pings(0){
            _last = Listener::getElapsedTimeMillis();
        }

        /**
         * call this for every iteration
         * returns true if one of the triggers has gone off
         */
        bool ping(){
            if ( ( ++_pings % _h ) == 0 ){
                _last = Listener::getElapsedTimeMillis();
                return true;
            }
            
            long long now = Listener::getElapsedTimeMillis();
            if ( now - _last > _ms ){
                _last = now;
                return true;
            }
                
            return false;
        }

    private:
        int _h;
        int _ms;

        unsigned long long _pings;

        long long _last;
        
    };
        
} // namespace mongo
