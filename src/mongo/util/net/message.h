// message.h

/*    Copyright 2009 10gen Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <vector>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/cstdint.h"
#include "mongo/base/data_view.h"
#include "mongo/base/encoded_value_storage.h"
#include "mongo/util/allocator.h"
#include "mongo/util/goodies.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sock.h"

namespace mongo {

    /**
     * Maximum accepted message size on the wire protocol.
     */
    const size_t MaxMessageSizeBytes = 48 * 1000 * 1000;

    class Message;
    class MessagingPort;
    class PiggyBackData;

    typedef uint32_t MSGID;

    enum Operations {
        opReply = 1,     /* reply. responseTo is set. */
        dbMsg = 1000,    /* generic msg command followed by a std::string */
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

    namespace MSGHEADER {
#pragma pack(1)
        /* see http://dochub.mongodb.org/core/mongowireprotocol
        */
        struct Layout {
            int32_t messageLength; // total message size, including this
            int32_t requestID;     // identifier for this message
            int32_t responseTo;    // requestID from the original request
            //   (used in responses from db)
            int32_t opCode;
        };
#pragma pack()

        class ConstView {
        public:
            typedef ConstDataView view_type;

            ConstView(const char* data) : _data(data) { }

            const char* view2ptr() const {
                return data().view();
            }

            int32_t getMessageLength() const {
                return data().readLE<int32_t>(offsetof(Layout, messageLength));
            }

            int32_t getRequestID() const {
                return data().readLE<int32_t>(offsetof(Layout, requestID));
            }

            int32_t getResponseTo() const {
                return data().readLE<int32_t>(offsetof(Layout, responseTo));
            }

            int32_t getOpCode() const {
                return data().readLE<int32_t>(offsetof(Layout, opCode));
            }

        protected:
            const view_type& data() const {
                return _data;
            }

        private:
            view_type _data;
        };

        class View : public ConstView {
        public:
            typedef DataView view_type;

            View(char* data) : ConstView(data) {}

            using ConstView::view2ptr;
            char* view2ptr() {
                return data().view();
            }

            void setMessageLength(int32_t value) {
                return data().writeLE(value, offsetof(Layout, messageLength));
            }

            void setRequestID(int32_t value) {
                return data().writeLE(value, offsetof(Layout, requestID));
            }

            void setResponseTo(int32_t value) {
                return data().writeLE(value, offsetof(Layout, responseTo));
            }

            void setOpCode(int32_t value) {
                return data().writeLE(value, offsetof(Layout, opCode));
            }

        private:
            view_type data() const {
                return const_cast<char *>(ConstView::view2ptr());
            }
        };

        class Value : public EncodedValueStorage<Layout, ConstView, View> {
        public:
            Value() {
                BOOST_STATIC_ASSERT(sizeof(Value) == sizeof(Layout));
            }

            Value(ZeroInitTag_t zit) : EncodedValueStorage<Layout, ConstView, View>(zit) {}
        };

    } // namespace MSGHEADER

    namespace MsgData {
#pragma pack(1)
        struct Layout {
            MSGHEADER::Layout header;
            char data[4];
        };
#pragma pack()

        class ConstView {
        public:
            ConstView(const char* storage) : _storage(storage) { }

            const char* view2ptr() const {
                return storage().view();
            }

            int32_t getLen() const {
                return header().getMessageLength();
            }

            MSGID getId() const {
                return header().getRequestID();
            }

            MSGID getResponseTo() const {
                return header().getResponseTo();
            }

            int32_t getOperation() const {
                return header().getOpCode();
            }

            const char* data() const {
                return storage().view(offsetof(Layout, data));
            }

            bool valid() const {
                if ( getLen() <= 0 || getLen() > ( 4 * BSONObjMaxInternalSize ) )
                    return false;
                if ( getOperation() < 0 || getOperation() > 30000 )
                    return false;
                return true;
            }

            int64_t getCursor() const {
                verify( getResponseTo() > 0 );
                verify( getOperation() == opReply );
                return ConstDataView(data() + sizeof(int32_t)).readLE<int64_t>();
            }

            int dataLen() const; // len without header

        protected:
            const ConstDataView& storage() const {
                return _storage;
            }

            MSGHEADER::ConstView header() const {
                return storage().view(offsetof(Layout, header));
            }

        private:
            ConstDataView _storage;
        };

        class View : public ConstView {
        public:
            View(char* storage) : ConstView(storage) {}

            using ConstView::view2ptr;
            char* view2ptr() {
                return storage().view();
            }

            void setLen(int value) {
                return header().setMessageLength(value);
            }

            void setId(MSGID value) {
                return header().setRequestID(value);
            }

            void setResponseTo(MSGID value) {
                return header().setResponseTo(value);
            }

            void setOperation(int value) {
                return header().setOpCode(value);
            }

            using ConstView::data;
            char* data() {
                return storage().view(offsetof(Layout, data));
            }

        private:
            DataView storage() const {
                return const_cast<char *>(ConstView::view2ptr());
            }

            MSGHEADER::View header() const {
                return storage().view(offsetof(Layout, header));
            }
        };

        class Value : public EncodedValueStorage<Layout, ConstView, View> {
        public:
            Value() {
                BOOST_STATIC_ASSERT(sizeof(Value) == sizeof(Layout));
            }

            Value(ZeroInitTag_t zit) : EncodedValueStorage<Layout, ConstView, View>(zit) {}
        };

        const int MsgDataHeaderSize = sizeof(Value) - 4;
        inline int ConstView::dataLen() const {
            return getLen() - MsgDataHeaderSize;
        }
    } // namespace MsgData

    class Message {
    public:
        // we assume here that a vector with initial size 0 does no allocation (0 is the default, but wanted to make it explicit).
        Message() : _buf( 0 ), _data( 0 ), _freeIt( false ) {}
        Message( void * data , bool freeIt ) :
            _buf( 0 ), _data( 0 ), _freeIt( false ) {
            _setData( reinterpret_cast< char* >( data ), freeIt );
        };
        Message(Message& r) : _buf( 0 ), _data( 0 ), _freeIt( false ) {
            *this = r;
        }
        ~Message() {
            reset();
        }

        SockAddr _from;

        MsgData::View header() const {
            verify( !empty() );
            return _buf ? _buf : _data[ 0 ].first;
        }

        int operation() const { return header().getOperation(); }

        MsgData::View singleData() const {
            massert( 13273, "single data buffer expected", _buf );
            return header();
        }

        bool empty() const { return !_buf && _data.empty(); }

        int size() const {
            int res = 0;
            if ( _buf ) {
                res =  MsgData::ConstView(_buf).getLen();
            }
            else {
                for (MsgVec::const_iterator it = _data.begin(); it != _data.end(); ++it) {
                    res += it->second;
                }
            }
            return res;
        }

        int dataSize() const { return size() - sizeof(MSGHEADER::Value); }

        // concat multiple buffers - noop if <2 buffers already, otherwise can be expensive copy
        // can get rid of this if we make response handling smarter
        void concat() {
            if ( _buf || empty() ) {
                return;
            }

            verify( _freeIt );
            int totalSize = 0;
            for (std::vector< std::pair< char *, int > >::const_iterator i = _data.begin();
                 i != _data.end(); ++i) {
                totalSize += i->second;
            }
            char *buf = (char*)mongoMalloc( totalSize );
            char *p = buf;
            for (std::vector< std::pair< char *, int > >::const_iterator i = _data.begin();
                 i != _data.end(); ++i) {
                memcpy( p, i->first, i->second );
                p += i->second;
            }
            reset();
            _setData( buf, true );
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
                for (std::vector< std::pair< char *, int > >::const_iterator i = _data.begin();
                     i != _data.end(); ++i) {
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
                MsgData::View md = d;
                md.setLen(size); // can be updated later if more buffers added
                _setData( md.view2ptr(), true );
                return;
            }
            verify( _freeIt );
            if ( _buf ) {
                _data.push_back(std::make_pair(_buf, MsgData::ConstView(_buf).getLen()));
                _buf = 0;
            }
            _data.push_back(std::make_pair(d, size));
            header().setLen(header().getLen() + size);
        }

        // use to set first buffer if empty
        void setData(char* d, bool freeIt) {
            verify( empty() );
            _setData( d, freeIt );
        }
        void setData(int operation, const char *msgtxt) {
            setData(operation, msgtxt, strlen(msgtxt)+1);
        }
        void setData(int operation, const char *msgdata, size_t len) {
            verify( empty() );
            size_t dataLen = len + sizeof(MsgData::Value) - 4;
            MsgData::View d = reinterpret_cast<char *>(mongoMalloc(dataLen));
            memcpy(d.data(), msgdata, len);
            d.setLen(dataLen);
            d.setOperation(operation);
            _setData( d.view2ptr(), true );
        }

        bool doIFreeIt() {
            return _freeIt;
        }

        void send( MessagingPort &p, const char *context );
        
        std::string toString() const;

    private:
        void _setData( char* d, bool freeIt ) {
            _freeIt = freeIt;
            _buf = d;
        }
        // if just one buffer, keep it in _buf, otherwise keep a sequence of buffers in _data
        char* _buf;
        // byte buffer(s) - the first must contain at least a full MsgData unless using _buf for storage instead
        typedef std::vector< std::pair< char*, int > > MsgVec;
        MsgVec _data;
        bool _freeIt;
    };


    MSGID nextMessageId();


} // namespace mongo
