// @file curop.h

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

#include <vector>

#include "mongo/bson/util/atomic_int.h"
#include "mongo/db/client.h"
#include "mongo/db/namespace-inl.h"
#include "mongo/util/concurrency/spin_lock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/progress_meter.h"
#include "mongo/util/time_support.h"

namespace mongo {

    class CurOp;

    /* lifespan is different than CurOp because of recursives with DBDirectClient */
    class OpDebug {
    public:
        OpDebug() : ns(""){ reset(); }

        void reset();

        void recordStats();

        string report( const CurOp& curop ) const;

        /**
         * Appends stored data and information from curop to the builder.
         *
         * @param curop information about the current operation which will be
         *     use to append data to the builder.
         * @param builder the BSON builder to use for appending data. Data can
         *     still be appended even if this method returns false.
         * @param maxSize the maximum allowed combined size for the query object
         *     and update object
         *
         * @return false if the sum of the sizes for the query object and update
         *     object exceeded maxSize
         */
        bool append(const CurOp& curop, BSONObjBuilder& builder, size_t maxSize) const;

        // -------------------
        
        StringBuilder extra; // weird things we need to fix later
        
        // basic options
        int op;
        bool iscommand;
        Namespace ns;
        BSONObj query;
        BSONObj updateobj;
        
        // detailed options
        long long cursorid;
        int ntoreturn;
        int ntoskip;
        bool exhaust;

        // debugging/profile info
        long long nscanned;
        bool idhack;         // indicates short circuited code path on an update to make the update faster
        bool scanAndOrder;   // scanandorder query plan aspect was used
        long long  nupdated; // number of records updated
        long long  nmoved;   // updates resulted in a move (moves are expensive)
        long long  ninserted;
        long long  ndeleted;
        bool fastmod;
        bool fastmodinsert;  // upsert of an $operation. builds a default object
        bool upsert;         // true if the update actually did an insert
        int keyUpdates;

        // error handling
        ExceptionInfo exceptionInfo;
        
        // response info
        int executionTime;
        int nreturned;
        int responseLength;
    };

    /**
     * stores a copy of a bson obj in a fixed size buffer
     * if its too big for the buffer, says "too big"
     * useful for keeping a copy around indefinitely without wasting a lot of space or doing malloc
     */
    class CachedBSONObj {
    public:
        enum { TOO_BIG_SENTINEL = 1 } ;
        static BSONObj _tooBig; // { $msg : "query not recording (too large)" }

        CachedBSONObj() {
            _size = (int*)_buf;
            reset();
        }

        void reset( int sz = 0 ) {
            _lock.lock();
            _reset( sz );
            _lock.unlock();
        }

        void set( const BSONObj& o ) {
            scoped_spinlock lk(_lock);
            int sz = o.objsize();
            if ( sz > (int) sizeof(_buf) ) {
                _reset(TOO_BIG_SENTINEL);
            }
            else {
                memcpy(_buf, o.objdata(), sz );
            }
        }

        int size() const { return *_size; }
        bool have() const { return size() > 0; }

        BSONObj get() const {
            scoped_spinlock lk(_lock);
            return _get();
        }

        void append( BSONObjBuilder& b , const StringData& name ) const {
            scoped_spinlock lk(_lock);
            BSONObj temp = _get();
            b.append( name , temp );
        }

    private:
        /** you have to be locked when you call this */
        BSONObj _get() const {
            int sz = size();
            if ( sz == 0 )
                return BSONObj();
            if ( sz == TOO_BIG_SENTINEL )
                return _tooBig;
            return BSONObj( _buf ).copy();
        }

        /** you have to be locked when you call this */
        void _reset( int sz ) { _size[0] = sz; }

        mutable SpinLock _lock;
        int * _size;
        char _buf[512];
    };

    /* Current operation (for the current Client).
       an embedded member of Client class, and typically used from within the mutex there.
    */
    class CurOp : boost::noncopyable {
    public:
        CurOp( Client * client , CurOp * wrapped = 0 );
        ~CurOp();

        bool haveQuery() const { return _query.have(); }
        BSONObj query() const { return _query.get();  }
        void appendQuery( BSONObjBuilder& b , const StringData& name ) const { _query.append( b , name ); }
        
        void ensureStarted();
        bool isStarted() const { return _start > 0; }
        void enter( Client::Context * context );
        void leave( Client::Context * context );
        void reset();
        void reset( const HostAndPort& remote, int op );
        void markCommand() { _command = true; }
        OpDebug& debug()           { return _debug; }
        int profileLevel() const   { return _dbprofile; }
        const char * getNS() const { return _ns; }

        bool shouldDBProfile( int ms ) const {
            if ( _dbprofile <= 0 )
                return false;

            return _dbprofile >= 2 || ms >= cmdLine.slowMS;
        }

        AtomicUInt opNum() const { return _opNum; }

        /** if this op is running */
        bool active() const { return _active; }

        bool displayInCurop() const { return _active && ! _suppressFromCurop; }
        int getOp() const { return _op; }
        unsigned long long startTime() { // micros
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
        int totalTimeMillis() { return (int) (totalTimeMicros() / 1000); }
        int elapsedMillis() {
            unsigned long long total = curTimeMicros64() - startTime();
            return (int) (total / 1000);
        }
        int elapsedSeconds() { return elapsedMillis() / 1000; }
        void setQuery(const BSONObj& query) { _query.set( query ); }
        Client * getClient() const { return _client; }
        BSONObj info();
        string getRemoteString( bool includePort = true ) { return _remote.toString(includePort); }
        ProgressMeter& setMessage(const char * msg,
                                  std::string name = "Progress",
                                  unsigned long long progressMeterTotal = 0,
                                  int secondsBetween = 3);
        string getMessage() const { return _message.toString(); }
        ProgressMeter& getProgressMeter() { return _progressMeter; }
        CurOp *parent() const { return _wrapped; }
        void kill(bool* pNotifyFlag = NULL); 
        bool killPendingStrict() const { return _killPending.load(); }
        bool killPending() const { return _killPending.loadRelaxed(); }
        void yielded() { _numYields++; }
        int numYields() const { return _numYields; }
        void suppressFromCurop() { _suppressFromCurop = true; }
        
        long long getExpectedLatencyMs() const { return _expectedLatencyMs; }
        void setExpectedLatencyMs( long long latency ) { _expectedLatencyMs = latency; }

        void recordGlobalTime( long long micros ) const;
        
        const LockStat& lockStat() const { return _lockStat; }
        LockStat& lockStat() { return _lockStat; }

        void setKillWaiterFlags();
    private:
        friend class Client;
        void _reset();

        static AtomicUInt _nextOpNum;
        Client * _client;
        CurOp * _wrapped;
        unsigned long long _start;
        unsigned long long _end;
        bool _active;
        bool _suppressFromCurop; // unless $all is set
        int _op;
        bool _command;
        int _dbprofile;                  // 0=off, 1=slow, 2=all
        AtomicUInt _opNum;               // todo: simple being "unsigned" may make more sense here
        char _ns[Namespace::MaxNsLen+2];
        HostAndPort _remote;             // CAREFUL here with thread safety
        CachedBSONObj _query;            // CachedBSONObj is thread safe
        OpDebug _debug;
        ThreadSafeString _message;
        ProgressMeter _progressMeter;
        AtomicInt32 _killPending;
        int _numYields;
        LockStat _lockStat;
        // _notifyList is protected by the global killCurrentOp's mtx.
        std::vector<bool*> _notifyList;
        
        // this is how much "extra" time a query might take
        // a writebacklisten for example will block for 30s 
        // so this should be 30000 in that case
        long long _expectedLatencyMs; 
    };
}
