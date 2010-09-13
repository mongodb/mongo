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
#include "../util/concurrency/spin_lock.h"
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
    
    class CachedBSONObj {
    public:
        enum { TOO_BIG_SENTINEL = 1 } ;
        static BSONObj _tooBig; // { $msg : "query not recording (too large)" }

        CachedBSONObj(){
            _size = (int*)_buf;
            reset();
        }
        
        void reset( int sz = 0 ){
            _size[0] = sz;
        }
        
        void set( const BSONObj& o ){
            _lock.lock();
            try {
                int sz = o.objsize();
                
                if ( sz > (int) sizeof(_buf) ) { 
                    reset(TOO_BIG_SENTINEL);
                }
                else {
                    memcpy(_buf, o.objdata(), sz );
                }
                
                _lock.unlock();
            }
            catch ( ... ){
                _lock.unlock();
                throw;
            }
            
        }
        
        int size() const { return *_size; }
        bool have() const { return size() > 0; }

        BSONObj get( bool threadSafe ){
            _lock.lock();
            
            BSONObj o;

            try {
                o = _get( threadSafe );
                _lock.unlock();
            }
            catch ( ... ){
                _lock.unlock();
                throw;
            }
            
            return o;
            
        }

        void append( BSONObjBuilder& b , const StringData& name ){
            _lock.lock();
            try {
                b.append( name , _get( false ) );
                _lock.unlock();
            }
            catch ( ... ){
                _lock.unlock();
                throw;
            }
        }
        
    private:

        /** you have to be locked when you call this */
        BSONObj _get( bool getCopy ){
            int sz = size();
            if ( sz == 0 )
                return BSONObj();
            if ( sz == TOO_BIG_SENTINEL )
                return _tooBig;
            return BSONObj( _buf ).copy();
        }

        SpinLock _lock;
        char _buf[512];
        int * _size;
    };

    /* Current operation (for the current Client).
       an embedded member of Client class, and typically used from within the mutex there. */
    class CurOp : boost::noncopyable {
        static AtomicUInt _nextOpNum;
        
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
        CachedBSONObj _query;
        
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
        
        bool haveQuery() const { return _query.have(); }
        BSONObj query( bool threadSafe = false ){ return _query.get( threadSafe );  }

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
            _query.reset();
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
            _query.set( query ); 
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
        
        BSONObj infoNoauth();

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
