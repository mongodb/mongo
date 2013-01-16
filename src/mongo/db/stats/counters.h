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

#include "mongo/pch.h"
#include "../jsobj.h"
#include "../../util/net/message.h"
#include "../../util/processinfo.h"
#include "../../util/concurrency/spin_lock.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    /**
     * for storing operation counters
     * note: not thread safe.  ok with that for speed
     */
    class OpCounters {
    public:

        OpCounters();
        void incInsertInWriteLock(int n) { _insert.x += n; }
        void gotInsert() { _insert++; }
        void gotQuery() { _query++; }
        void gotUpdate() { _update++; }
        void gotDelete() { _delete++; }
        void gotGetMore() { _getmore++; }
        void gotCommand() { _command++; }

        void gotOp( int op , bool isCommand );

        BSONObj getObj() const;
        
        // thse are used by snmp, and other things, do not remove
        const AtomicUInt * getInsert() const { return &_insert; }
        const AtomicUInt * getQuery() const { return &_query; }
        const AtomicUInt * getUpdate() const { return &_update; }
        const AtomicUInt * getDelete() const { return &_delete; }
        const AtomicUInt * getGetMore() const { return &_getmore; }
        const AtomicUInt * getCommand() const { return &_command; }


    private:
        void _checkWrap();
        
        // todo: there will be a lot of cache line contention on these.  need to do something 
        //       else eventually.
        AtomicUInt _insert;
        AtomicUInt _query;
        AtomicUInt _update;
        AtomicUInt _delete;
        AtomicUInt _getmore;
        AtomicUInt _command;
    };

    extern OpCounters globalOpCounters;
    extern OpCounters replOpCounters;

    class NetworkCounter {
    public:
        NetworkCounter() : _bytesIn(0), _bytesOut(0), _requests(0), _overflows(0) {}
        void hit( long long bytesIn , long long bytesOut );
        void append( BSONObjBuilder& b );
    private:
        long long _bytesIn;
        long long _bytesOut;
        long long _requests;

        long long _overflows;

        SpinLock _lock;
    };

    extern NetworkCounter networkCounter;
}
