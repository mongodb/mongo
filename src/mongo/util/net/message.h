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

#include "sock.h"
#include "hostandport.h"

namespace mongo {

    class Message;
    class MessagingPort;
    class PiggyBackData;

    typedef unsigned int MSGID;

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

    inline const char * opToString( int op ) {
        switch ( op ) {
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
            massert( 16141, str::stream() << "cannot translate opcode " << op, !op );
            return "";
        }
    }

    inline bool opIsWrite( int op ) {
        switch ( op ) {

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
            return true;

        default:
            PRINT(op);
            verify(0);
            return "";
        }

    }

#pragma pack(1)
    /* see http://www.mongodb.org/display/DOCS/Mongo+Wire+Protocol
    */
    struct MSGHEADER {
        little<int> messageLength; // total message size, including this
        little<int> requestID;     // identifier for this message
        little<int> responseTo;    // requestID from the original request
        //   (used in reponses from db)
        little<int> opCode;
    };
#pragma pack()

#pragma pack(1)
    /* todo merge this with MSGHEADER (or inherit from it). */
    struct MsgData {
        little<int> len; /* len of the msg, including this field */
        little<MSGID> id; /* request/reply id's match... */
        little<MSGID> responseTo; /* id of the message we are responding to */
        little<short> _operation;
        char _flags;
        char _version;
        int operation() const {
            return _operation;
        }
        void setOperation(int o) {
            _flags = 0;
            _version = 0;
            _operation = o;
        }
        char _data[4];

        little<int>& dataAsInt() {
            return little<int>::ref( _data );
        }

        bool valid() {
            if ( len <= 0 || len > ( 4 * BSONObjMaxInternalSize ) )
                return false;
            if ( _operation < 0 || _operation > 30000 )
                return false;
            return true;
        }

        long long getCursor() {
            verify( responseTo > 0 );
            verify( _operation == opReply );
            return little<long long>::ref( _data + 4 );
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
            verify( !empty() );
            return _buf ? _buf : reinterpret_cast< MsgData* > ( _data[ 0 ].first );
        }
        int operation() const { return header()->operation(); }

        MsgData *singleData() const {
            massert( 13273, "single data buffer expected", _buf );
            return header();
        }

        bool empty() const { return !_buf && _data.empty(); }

        int size() const {
            int res = 0;
            if ( _buf ) {
                res =  _buf->len;
            }
            else {
                for (MsgVec::const_iterator it = _data.begin(); it != _data.end(); ++it) {
                    res += it->second;
                }
            }
            return res;
        }

        int dataSize() const { return size() - sizeof(MSGHEADER); }

        // concat multiple buffers - noop if <2 buffers already, otherwise can be expensive copy
        // can get rid of this if we make response handling smarter
        void concat() {
            if ( _buf || empty() ) {
                return;
            }

            verify( _freeIt );
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
            verify( empty() );
            verify( r._freeIt );
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
            verify( _freeIt );
            if ( _buf ) {
                _data.push_back( make_pair( (char*)_buf, _buf->len ) );
                _buf = 0;
            }
            _data.push_back( make_pair( d, size ) );
            header()->len += size;
        }

        // use to set first buffer if empty
        void setData(MsgData *d, bool freeIt) {
            verify( empty() );
            _setData( d, freeIt );
        }
        void setData(int operation, const char *msgtxt) {
            setData(operation, msgtxt, strlen(msgtxt)+1);
        }
        void setData(int operation, const char *msgdata, size_t len) {
            verify( empty() );
            size_t dataLen = len + sizeof(MsgData) - 4;
            MsgData *d = (MsgData *) malloc(dataLen);
            memcpy(d->_data, msgdata, len);
            d->len = dataLen;
            d->setOperation(operation);
            _setData( d, true );
        }

        bool doIFreeIt() {
            return _freeIt;
        }

        void send( MessagingPort &p, const char *context );
        
        string toString() const;

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


    MSGID nextMessageId();


} // namespace mongo
