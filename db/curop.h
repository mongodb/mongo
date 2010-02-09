// curop.h

#pragma once

#include "namespace.h"
#include "client.h"
#include "../util/atomic_int.h"
#include "db.h"

namespace mongo { 

    class OpDebug {
    public:
        StringBuilder str;
        
        void reset(){
            str.reset();
        }
    };
    
    /* Current operation (for the current Client).
       an embedded member of Client class, and typically used from within the mutex there. */
    class CurOp : boost::noncopyable {
        static AtomicUInt _nextOpNum;
        static BSONObj _tooBig; // { $msg : "query not recording (too large)" }
        
        Client * _client;
        CurOp * _wrapped;

        unsigned long long _start;
        unsigned long long _checkpoint;
        unsigned long long _end;

        bool _active;
        int _op;
        int _lockType; // see concurrency.h for values
        bool _waitingForLock;
        int _dbprofile; // 0=off, 1=slow, 2=all
        AtomicUInt _opNum;
        char _ns[Namespace::MaxNsLen+2];
        struct sockaddr_in _remote;
        
        char _queryBuf[256];

        void resetQuery(int x=0) { *((int *)_queryBuf) = x; }
        
        OpDebug _debug;

        void _reset(){
            _lockType = 0;
            _dbprofile = 0;
            _end = 0;
            _waitingForLock = false;
        }

        void setNS(const char *ns) {
            strncpy(_ns, ns, Namespace::MaxNsLen);
        }

    public:

        bool haveQuery() const { return *((int *) _queryBuf) != 0; }

        BSONObj query() {
            if( *((int *) _queryBuf) == 1 ) { 
                return _tooBig;
            }
            BSONObj o(_queryBuf);
            return o;
        }

        void ensureStarted(){
            if ( _start == 0 )
                _start = _checkpoint = curTimeMicros64();            
        }
        void enter( Client::Context * context ){
            ensureStarted();
            setNS( context->ns() );
            if ( context->_db && context->_db->profile > _dbprofile )
                _dbprofile = context->_db->profile;
        }

        void leave( Client::Context * context ){
            unsigned long long now = curTimeMicros64();
            Top::global.record( _ns , _op , _lockType , now - _checkpoint );
            _checkpoint = now;
        }
        
        void reset( const sockaddr_in & remote, int op ) { 
            _reset();
            _start = _checkpoint = 0;
            _active = true;
            _opNum = _nextOpNum++;
            _ns[0] = '?'; // just in case not set later
            _debug.reset();
            resetQuery();
            _remote = remote;
            _op = op;
        }

        void setRead(){
            _lockType = -1;
        }
        void setWrite(){
            _lockType = 1;
        }

        void waitingForLock(){
            _waitingForLock = true;
        }
        void gotLock(){
            _waitingForLock = false;
        }

        OpDebug& debug(){
            return _debug;
        }
        
        int profileLevel() const {
            return _dbprofile;
        }

        const char * getNS() const {
            return _ns;
        }

        bool shouldDBProfile( int ms ) const {
            if ( _dbprofile <= 0 )
                return false;
            
            return _dbprofile >= 2 || ms >= cmdLine.slowMS;
        }
        
        AtomicUInt opNum() const { return _opNum; }
        bool active() const { return _active; }
        int getLockType() const { return _lockType; }
        bool isWaitingForLock() const { return _waitingForLock; } 
        int getOp() const { return _op; }

        /** micros */
        unsigned long long startTime() {
            ensureStarted();
            return _start;
        }

        void done() {
            _active = false;
            _end = curTimeMicros64();
        }
        
        unsigned long long totalTimeMicros() {
            massert( 12601 , "CurOp not marked done yet" , ! _active );
            return _end - startTime();
        }

        int totalTimeMillis() {
            return (int) (totalTimeMicros() / 1000);
        }

        int elapsedMillis() {
            unsigned long long total = curTimeMicros64() - startTime();
            return (int) (total / 1000);
        }

        int elapsedSeconds() {
            return elapsedMillis() / 1000;
        }

        void setQuery(const BSONObj& query) { 
            if( query.objsize() > (int) sizeof(_queryBuf) ) { 
                resetQuery(1); // flag as too big and return
                return;
            }
            memcpy(_queryBuf, query.objdata(), query.objsize());
        }

        CurOp( Client * client , CurOp * wrapped = 0 ) { 
            _client = client;
            _wrapped = wrapped;
            if ( _wrapped ){
                _client->_curOp = this;
            }
            _start = _checkpoint = 0;
            _active = false;
            _reset();
            _op = 0;
            // These addresses should never be written to again.  The zeroes are
            // placed here as a precaution because currentOp may be accessed
            // without the db mutex.
            memset(_ns, 0, sizeof(_ns));
            memset(_queryBuf, 0, sizeof(_queryBuf));
        }
        
        ~CurOp(){
            if ( _wrapped )
                _client->_curOp = _wrapped;
        }

        BSONObj info() { 
            if( ! cc().getAuthenticationInfo()->isAuthorized("admin") ) { 
                BSONObjBuilder b;
                b.append("err", "unauthorized");
                return b.obj();
            }
            return infoNoauth();
        }
        
        BSONObj infoNoauth();

        string getRemoteString(){
            stringstream ss;
            ss << inet_ntoa( _remote.sin_addr ) << ":" << ntohs( _remote.sin_port );
            return ss.str();
        }

        friend class Client;
    };

    /* 0 = ok
       1 = kill current operation and reset this to 0
       future: maybe use this as a "going away" thing on process termination with a higher flag value 
    */
    extern class KillCurrentOp { 
         enum { Off, On, All } state;
        AtomicUInt toKill;
    public:
        void killAll() { state = All; }
        void kill(AtomicUInt i) { toKill = i; state = On; }

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
