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
*/


#pragma once

#include "mongo/db/commands/server_status.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/record.h"
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
