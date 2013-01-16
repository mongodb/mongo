// @file queryutil-inl.h - Inline definitions for frequently called queryutil.h functions

/*    Copyright 2011 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

namespace mongo {
    
    inline bool FieldInterval::equality() const {
        if ( _cachedEquality == -1 ) {
            _cachedEquality = ( _lower._inclusive && _upper._inclusive && _lower._bound.woCompare( _upper._bound, false ) == 0 );
        }
        return _cachedEquality != 0;
    }

    inline bool FieldRange::equality() const {
        return
            !empty() &&
            min().woCompare( max(), false ) == 0 &&
            maxInclusive() &&
            minInclusive();
    }

    inline const FieldRange &FieldRangeSet::range( const char *fieldName ) const {
        map<string,FieldRange>::const_iterator f = _ranges.find( fieldName );
        if ( f == _ranges.end() )
            return universalRange();
        return f->second;
    }

    inline FieldRange &FieldRangeSet::range( const char *fieldName ) {
        map<string,FieldRange>::iterator f = _ranges.find( fieldName );
        if ( f == _ranges.end() ) {
            _ranges.insert( make_pair( string( fieldName ), universalRange() ) );
            return _ranges.find( fieldName )->second;
        }
        return f->second;
    }

    inline bool FieldRangeSet::matchPossible() const {
        for( map<string,FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            if ( i->second.empty() ) {
                return false;
            }
        }
        return true;
    }
    
    inline bool FieldRangeSet::matchPossibleForIndex( const BSONObj &keyPattern ) const {
        if ( !_singleKey ) {
            return matchPossible();   
        }
        BSONObjIterator i( keyPattern );
        while( i.more() ) {
            BSONElement e = i.next();
            if ( e.fieldName() == string( "$natural" ) ) {
                return true;
            }
            if ( range( e.fieldName() ).empty() ) {
                return false;
            }
        }
        return true;
    }

    inline unsigned FieldRangeVector::size() const {
        unsigned ret = 1;
        for( vector<FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            unsigned long long product =
                    (unsigned long long)ret * (unsigned long long)i->intervals().size();
            // Check for overflow SERVER-5415.
            verify( ( product >> 32 ) == 0 );
            ret = static_cast<unsigned>( product );
        }
        return ret;
    }
    
    inline void FieldRangeVectorIterator::CompoundRangeCounter::set( int i, int newVal ) {
        resetIntervalCount();
        _i[ i ] = newVal;
    }
    
    inline void FieldRangeVectorIterator::CompoundRangeCounter::inc( int i ) {
        resetIntervalCount();
        ++_i[ i ];
    }
    
    inline void FieldRangeVectorIterator::CompoundRangeCounter::setZeroes( int i ) {
        resetIntervalCount();
        for( int j = i; j < (int)_i.size(); ++j ) _i[ j ] = 0;
    }
    
    inline void FieldRangeVectorIterator::CompoundRangeCounter::setUnknowns( int i ) {
        resetIntervalCount();
        for( int j = i; j < (int)_i.size(); ++j ) _i[ j ] = -1;
    }

    inline FieldRangeSetPair *OrRangeGenerator::topFrsp() const {
        FieldRangeSetPair *ret = new FieldRangeSetPair( _baseSet );
        if (_orSets.size()) {
            *ret &= _orSets.front();
        }
        return ret;
    }

    inline FieldRangeSetPair *OrRangeGenerator::topFrspOriginal() const {
        FieldRangeSetPair *ret = new FieldRangeSetPair( _baseSet );
        if (_originalOrSets.size()) {
            *ret &= _originalOrSets.front();
        }
        return ret;
    }
    
    inline bool FieldRangeSetPair::matchPossibleForIndex( NamespaceDetails *d, int idxNo, const BSONObj &keyPattern ) const {
        assertValidIndexOrNoIndex( d, idxNo );
        if ( !matchPossible() ) {
            return false;
        }
        if ( idxNo < 0 ) {
            // multi key matchPossible() is true, so return true.
            return true;   
        }
        return frsForIndex( d, idxNo ).matchPossibleForIndex( keyPattern );
    }

    inline void FieldRangeSetPair::assertValidIndexOrNoIndex( const NamespaceDetails *d, int idxNo ) const {
        massert( 14049, "FieldRangeSetPair invalid index specified", idxNo >= -1 );
        if ( idxNo >= 0 ) {
            assertValidIndex( d, idxNo );   
        }
    }        
    
} // namespace mongo
