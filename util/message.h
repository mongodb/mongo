// message.h

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

#include "../util/sock.h"

namespace mongo {

    class Message;
    class MessagingPort;
    class PiggyBackData;
    typedef WrappingInt MSGID;

    class Listener {
    public:
        Listener(const string &_ip, int p) : ip(_ip), port(p) { }
        virtual ~Listener() {}
        bool init(); // set up socket
        int socket() const { return sock; }
        void listen(); // never returns (start a thread)

        /* spawn a thread, etc., then return */
        virtual void accepted(MessagingPort *mp) = 0;
    private:
        string ip;
        int port;
        int sock;
    };

    class AbstractMessagingPort {
    public:
        virtual ~AbstractMessagingPort() { }
        virtual void reply(Message& received, Message& response, MSGID responseTo) = 0; // like the reply below, but doesn't rely on received.data still being available
        virtual void reply(Message& received, Message& response) = 0;
        
        virtual unsigned remotePort() = 0 ;
    };

    class MessagingPort : public AbstractMessagingPort {
    public:
        MessagingPort(int sock, SockAddr& farEnd);
        MessagingPort();
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

        virtual unsigned remotePort();
    private:
        int sock;
        PiggyBackData * piggyBackData;
    public:
        SockAddr farEnd;

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
            data = 0;
            freeIt = false;
        }
        Message( void * _data , bool _freeIt ) {
            data = (MsgData*)_data;
            freeIt = _freeIt;
        };
        ~Message() {
            reset();
        }

        SockAddr from;
        MsgData *data;

        Message& operator=(Message& r) {
            assert( data == 0 );
            data = r.data;
            assert( r.freeIt );
            r.freeIt = false;
            r.data = 0;
            freeIt = true;
            return *this;
        }

        void reset() {
            if ( freeIt && data )
                free(data);
            data = 0;
            freeIt = false;
        }

        void setData(MsgData *d, bool _freeIt) {
            assert( data == 0 );
            freeIt = _freeIt;
            data = d;
        }
        void setData(int operation, const char *msgtxt) {
            setData(operation, msgtxt, strlen(msgtxt)+1);
        }
        void setData(int operation, const char *msgdata, int len) {
            assert(data == 0);
            int dataLen = len + sizeof(MsgData) - 4;
            MsgData *d = (MsgData *) malloc(dataLen);
            memcpy(d->_data, msgdata, len);
            d->len = fixEndian(dataLen);
            d->setOperation(operation);
            freeIt= true;
            data = d;
        }

        bool doIFreeIt() {
            return freeIt;
        }

    private:
        bool freeIt;
    };

    class SocketException : public DBException {
    public:
        virtual const char* what() const throw() { return "socket exception"; }
    };

    MSGID nextMessageId();

    void setClientId( int id );
    int getClientId();
} // namespace mongo
