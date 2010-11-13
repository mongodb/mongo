// @file d_chunk_matcher.cpp

/**
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

#include "d_chunk_matcher.h"

namespace mongo {

    ChunkMatcher::ChunkMatcher( ShardChunkVersion version , const BSONObj& key ) : _version( version ) {
        BSONObjBuilder b;
        BSONForEach( e , key ) {
            b.append( e.fieldName() , 1 );
        }
        _key = b.obj();
    }

    void ChunkMatcher::addChunk( const BSONObj& min , const BSONObj& max ) {
        _chunksMap[min] == make_pair( min , max );
    }

    void ChunkMatcher::addRange( const BSONObj& min , const BSONObj& max ){
        //TODO debug mode only?
        assert(min.nFields() == _key.nFields());
        assert(max.nFields() == _key.nFields());

        _rangesMap[min] = make_pair(min,max);
    }

    bool ChunkMatcher::belongsToMe( const BSONObj& obj ) const {
        if ( _rangesMap.size() == 0 )
            return false;
        
        BSONObj x = obj.extractFields(_key);

        RangeMap::const_iterator a = _rangesMap.upper_bound( x );
        if ( a != _rangesMap.begin() )
            a--;
        
        bool good = x.woCompare( a->second.first ) >= 0 && x.woCompare( a->second.second ) < 0;
#if 0
        if ( ! good ){
            cout << "bad: " << x << "\t" << a->second.first << "\t" << x.woCompare( a->second.first ) << "\t" << x.woCompare( a->second.second ) << endl;
            for ( MyMap::const_iterator i=_map.begin(); i!=_map.end(); ++i ){
                cout << "\t" << i->first << "\t" << i->second.first << "\t" << i->second.second << endl;
            }
        }
#endif
        return good;
    }
    
}  // namespace mongo
