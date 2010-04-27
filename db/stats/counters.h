// counters.h
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

#include "../../pch.h"
#include "../jsobj.h"
#include "../../util/message.h"
#include "../../util/processinfo.h"

namespace mongo {

    /**
     * for storing operation counters
     * note: not thread safe.  ok with that for speed
     */
    class OpCounters {
    public:
        
        OpCounters();

        int * getInsert(){ return _insert; }
        int * getQuery(){ return _query; }
        int * getUpdate(){ return _update; }
        int * getDelete(){ return _delete; }
        int * getGetMore(){ return _getmore; }
        int * getCommand(){ return _command; }
        
        void gotInsert(){ _insert[0]++; }
        void gotQuery(){ _query[0]++; }
        void gotUpdate(){ _update[0]++; }
        void gotDelete(){ _delete[0]++; }
        void gotGetMore(){ _getmore[0]++; }
        void gotCommand(){ _command[0]++; }

        void gotOp( int op , bool isCommand );

        BSONObj& getObj(){ return _obj; }
    private:
        BSONObj _obj;
        int * _insert;
        int * _query;
        int * _update;
        int * _delete;
        int * _getmore;
        int * _command;
    };
    
    extern OpCounters globalOpCounters;

    class IndexCounters {
    public:
        IndexCounters();
        
        void btree( char * node ){
            if ( ! _memSupported )
                return;
            if ( _sampling++ % _samplingrate )
                return;
            btree( _pi.blockInMemory( node ) );
        }

        void btree( bool memHit ){
            if ( memHit )
                _btreeMemHits++;
            else
                _btreeMemMisses++;
            _btreeAccesses++;
        }
        void btreeHit(){ _btreeMemHits++; _btreeAccesses++; }
        void btreeMiss(){ _btreeMemMisses++; _btreeAccesses++; }
        
        void append( BSONObjBuilder& b );
        
    private:
        ProcessInfo _pi;
        bool _memSupported;

        int _sampling;
        int _samplingrate;
        
        int _resets;
        long long _maxAllowed;
        
        long long _btreeMemMisses;
        long long _btreeMemHits;
        long long _btreeAccesses;
    };

    extern IndexCounters globalIndexCounters;

    class FlushCounters {
    public:
        FlushCounters();

        void flushed(int ms);
        
        void append( BSONObjBuilder& b );

    private:
        long long _total_time;
        long long _flushes;
        int _last_time;
        Date_t _last;
    };

    extern FlushCounters globalFlushCounters;


    class GenericCounter {
    public:
        void hit( const string& name , int count=0 );
        BSONObj getObj();
    private:
        map<string,long long> _counts; // TODO: replace with thread safe map
        mongo::mutex _mutex;
    };
}
