// curop.h

#pragma once

#include "namespace.h"
#include "security.h"
#include "client.h"

namespace mongo { 

    /* Current operation (for the current Client).
       an embedded member of Client class, and typically used from within the mutex there. */
    class CurOp {
        static WrappingInt _nextOpNum;
        static BSONObj _tooBig; // { $msg : "query not recording (too large)" }

        bool _active;
        Timer _timer;
        int _op;
        WrappingInt _opNum;
        char _ns[Namespace::MaxNsLen+2];
        struct sockaddr_in client;

        char _queryBuf[256];
        bool haveQuery() const { return *((int *) _queryBuf) != 0; }
        void resetQuery(int x=0) { *((int *)_queryBuf) = x; }
        BSONObj query() {
            if( *((int *) _queryBuf) == 1 ) { 
                return _tooBig;
            }
            BSONObj o(_queryBuf);
            return o;
        }

    public:
        void reset( const sockaddr_in &_client) { 
            _active = true;
            _opNum = _nextOpNum.atomicIncrement();
            _timer.reset();
            _ns[0] = '?'; // just in case not set later
            resetQuery();
            client = _client;
        }

        WrappingInt opNum() const { return _opNum; }
        bool active() const { return _active; }

        int elapsedMillis(){ return _timer.millis(); }
        
        /** micros */
        unsigned long long startTime(){
            return _timer.startTime();
        }

        void setActive(bool active) { _active = active; }
        void setNS(const char *ns) {
            strncpy(_ns, ns, Namespace::MaxNsLen);
        }
        void setOp(int op) { _op = op; }
        void setQuery(const BSONObj& query) { 
            if( query.objsize() > (int) sizeof(_queryBuf) ) { 
                resetQuery(1); // flag as too big and return
                return;
            }
            memcpy(_queryBuf, query.objdata(), query.objsize());
        }

        CurOp() { 
            _active = false;
//            opNum = 0; 
            _op = 0;
            // These addresses should never be written to again.  The zeroes are
            // placed here as a precaution because currentOp may be accessed
            // without the db mutex.
            memset(_ns, 0, sizeof(_ns));
            memset(_queryBuf, 0, sizeof(_queryBuf));
        }

        BSONObj info() { 
            AuthenticationInfo *ai = currentClient.get()->ai;
            if( !ai->isAuthorized("admin") ) { 
                BSONObjBuilder b;
                b.append("err", "unauthorized");
                return b.obj();
            }
            return infoNoauth();
        }
        
        BSONObj infoNoauth() {
            BSONObjBuilder b;
            b.append("opid", _opNum);
            b.append("active", _active);
            if( _active ) 
                b.append("secs_running", _timer.seconds() );
            if( _op == 2004 ) 
                b.append("op", "query");
            else if( _op == 2005 )
                b.append("op", "getMore");
            else if( _op == 2001 )
                b.append("op", "update");
            else if( _op == 2002 )
                b.append("op", "insert");
            else if( _op == 2006 )
                b.append("op", "delete");
            else
                b.append("op", _op);
            b.append("ns", _ns);

            if( haveQuery() ) {
                b.append("query", query());
            }
            // b.append("inLock",  ??
            stringstream clientStr;
            clientStr << inet_ntoa( client.sin_addr ) << ":" << ntohs( client.sin_port );
            b.append("client", clientStr.str());
            return b.obj();
        }
    };

    /* 0 = ok
       1 = kill current operation and reset this to 0
       future: maybe use this as a "going away" thing on process termination with a higher flag value 
    */
    extern class KillCurrentOp { 
         enum { Off, On, All } state;
        WrappingInt toKill;
    public:
        void killAll() { state = All; }
        void kill(WrappingInt i) { toKill = i; state = On; }

        void checkForInterrupt() { 
            if( state != Off ) { 
                if( state == All ) 
                    uasserted(11600,"interrupted at shutdown");
                if( cc().curop()->opNum() == toKill ) { 
                    state = Off;
                    uasserted(11601,"interrupted");
                }
            }
        }
    } killCurrentOp;
}
