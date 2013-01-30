// btree_stats.cpp

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

#include "mongo/pch.h"

#include "mongo/db/btree_stats.h"

namespace mongo {


    IndexCounters::IndexCounters() 
        : ServerStatusSection( "indexCounters" ) {

        _memSupported = ProcessInfo().blockCheckSupported();

        _btreeMemHits = 0;
        _btreeMemMisses = 0;
        _btreeAccesses = 0;


        _maxAllowed = ( numeric_limits< long long >::max() ) / 2;
        _resets = 0;
    }

    IndexCounters::~IndexCounters(){
    }

    BSONObj IndexCounters::generateSection(const BSONElement& configElement) const {
        if ( ! _memSupported ) {
            return BSON( "note" << "not supported on this platform" );
        }
        
        BSONObjBuilder bb;
        bb.appendNumber( "accesses" , _btreeAccesses );
        bb.appendNumber( "hits" , _btreeMemHits );
        bb.appendNumber( "misses" , _btreeMemMisses );

        bb.append( "resets" , _resets );

        bb.append( "missRatio" , (_btreeAccesses ? (_btreeMemMisses / (double)_btreeAccesses) : 0) );

        return bb.obj();        
    }

    void IndexCounters::_roll() {
        _btreeAccesses = 0;
        _btreeMemMisses = 0;
        _btreeMemHits = 0;
        _resets++;
    }

    IndexCounters globalIndexCounters;

}
