// btree_stats.h

/**
*    Copyright (C) 2008-2012 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/


#pragma once

#include "mongo/db/commands/server_status.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/storage/record.h"
#include "mongo/util/processinfo.h"

namespace mongo {
    

    class IndexCounters : public ServerStatusSection {
    public:
        IndexCounters();
        virtual ~IndexCounters();

        virtual bool includeByDefault() const { return true; }

        virtual BSONObj generateSection(const BSONElement& configElement) const;


        // used without a mutex intentionally (can race)
        void btree( const char* node ) {
            if ( ! _memSupported )
                return;
            btree( Record::likelyInPhysicalMemory( node ) );
        }

        void btree( bool memHit ) {
            if ( memHit )
                _btreeMemHits++;
            else
                _btreeMemMisses++;
            if ( _btreeAccesses++ > _maxAllowed )
                _roll();

        }
        void btreeHit() { _btreeMemHits++; _btreeAccesses++; }
        void btreeMiss() { _btreeMemMisses++; _btreeAccesses++; }

    private:
        
        void _roll();

        bool _memSupported;

        int _resets;
        long long _maxAllowed;

        long long _btreeMemMisses;
        long long _btreeMemHits;
        long long _btreeAccesses;
    };

    extern IndexCounters* globalIndexCounters;
}
