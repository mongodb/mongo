// curop.h
/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "namespace.h"
#include "client.h"
#include "../bson/util/atomic_int.h"
#include "db.h"

namespace mongo { 

    /* lifespan is different than CurOp because of recursives with DBDirectClient */
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
        bool _command;
        int _lockType; // see concurrency.h for values
        bool _waitingForLock;
        int _dbprofile; // 0=off, 1=slow, 2=all
        AtomicUInt _opNum;
        char _ns[Namespace::MaxNsLen+2];
        struct SockAddr _remote;
        char _queryBuf[256];
        
        void resetQuery(int x=0) { *((int *)_queryBuf) = x; }
        
        OpDebug _debug;
        
        ThreadSafeString _message;
        ProgressMeter _progressMeter;

        void _reset(){
            _command = false;
            _lockType = 0;
            _dbprofile = 0;
            _end = 0;
            _waitingForLock = false;
            _message = "";
            _progressMeter.finished();
        }

        void setNS(const char *ns) {
            strncpy(_ns, ns, Namespace::MaxNsLen);
        }

    public:
        
        int querySize() const { return *((int *) _queryBuf); }
        bool haveQuery() const { return querySize() != 0; }

        BSONObj query( bool threadSafe = false);

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
            Top::global.record( _ns , _op , _lockType , now - _checkpoint , _command );
            _checkpoint = now;
        }

        void reset(){
            _reset();
            _start = _checkpoint = 0;
            _active = true;
            _opNum = _nextOpNum++;
            _ns[0] = '?'; // just in case not set later
            _debug.reset();
            resetQuery();            
        }
        
        void reset( const SockAddr & remote, int op ) {
            reset();
            _remote = remote;
            _op = op;
        }
        
        void markCommand(){
            _command = true;
        }

        void waitingForLock( int type ){
            _waitingForLock = true;
            if ( type > 0 )
                _lockType = 1;
            else
                _lockType = -1;
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

        /** if this op is running */
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

        Client * getClient() const { 
            return _client;
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
        
        ~CurOp();

        BSONObj info() { 
            if( ! cc().getAuthenticationInfo()->isAuthorized("admin") ) { 
                BSONObjBuilder b;
                b.append("err", "unauthorized");
                return b.obj();
            }
            return infoNoauth();
        }
        
        BSONObj infoNoauth( int attempt = 0 );

        string getRemoteString( bool includePort = true ){
            return _remote.toString(includePort);
        }

        ProgressMeter& setMessage( const char * msg , long long progressMeterTotal = 0 , int secondsBetween = 3 ){

            if ( progressMeterTotal ){
                if ( _progressMeter.isActive() ){
                    cout << "about to assert, old _message: " << _message << " new message:" << msg << endl;
                    assert( ! _progressMeter.isActive() );
                }
                _progressMeter.reset( progressMeterTotal , secondsBetween );
            }
            else {
                _progressMeter.finished();
            }
            
            _message = msg;
            
            return _progressMeter;
        }
        
        string getMessage() const { return _message.toString(); }
        ProgressMeter& getProgressMeter() { return _progressMeter; }

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
