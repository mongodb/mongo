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

    inline long long FieldRangeVector::size() {
        long long ret = 1;
        for( vector<FieldRange>::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            ret *= i->intervals().size();
        }
        return ret;
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
