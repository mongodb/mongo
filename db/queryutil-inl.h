// queryutil-inl.h - Inline definitions for frequently called queryutil.h functions

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
        return _cachedEquality;
    }

    inline bool FieldRange::equality() const {
        return
            !empty() &&
            min().woCompare( max(), false ) == 0 &&
            maxInclusive() &&
            minInclusive();
    }

    inline bool FieldRange::inQuery() const {
        if ( equality() ) {
            return true;
        }
        for( vector< FieldInterval >::const_iterator i = _intervals.begin(); i != _intervals.end(); ++i ) {
            if ( !i->equality() ) {
                return false;
            }
        }
        return true;
    }

    /**
     * TODO Assumes intervals are contiguous and minKey/maxKey will not be
     * matched against.
     */
    inline bool FieldRange::nontrivial() const {
        return
            ! empty() &&
            ( _intervals.size() != 1 ||
              minKey.firstElement().woCompare( min(), false ) != 0 ||
              maxKey.firstElement().woCompare( max(), false ) != 0 );
    }

    inline bool QueryPattern::operator<( const QueryPattern &other ) const {
        map< string, Type >::const_iterator i = _fieldTypes.begin();
        map< string, Type >::const_iterator j = other._fieldTypes.begin();
        while( i != _fieldTypes.end() ) {
            if ( j == other._fieldTypes.end() )
                return false;
            if ( i->first < j->first )
                return true;
            else if ( i->first > j->first )
                return false;
            if ( i->second < j->second )
                return true;
            else if ( i->second > j->second )
                return false;
            ++i;
            ++j;
        }
        if ( j != other._fieldTypes.end() )
            return true;
        return _sort.woCompare( other._sort ) < 0;
    }

    inline const FieldRange &FieldRangeSet::range( const char *fieldName ) const {
        map< string, FieldRange >::const_iterator f = _ranges.find( fieldName );
        if ( f == _ranges.end() )
            return trivialRange();
        return f->second;
    }

    inline FieldRange &FieldRangeSet::range( const char *fieldName ) {
        map< string, FieldRange >::iterator f = _ranges.find( fieldName );
        if ( f == _ranges.end() )
            return trivialRange();
        return f->second;
    }

    inline int FieldRangeSet::nNontrivialRanges() const {
        int count = 0;
        for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            if ( i->second.nontrivial() )
                ++count;
        }
        return count;
    }

    inline bool FieldRangeSet::matchPossible() const {
        for( map< string, FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i )
            if ( i->second.empty() )
                return false;
        return true;
    }

    inline long long FieldRangeVector::size() {
        long long ret = 1;
        for( vector< FieldRange >::const_iterator i = _ranges.begin(); i != _ranges.end(); ++i ) {
            ret *= i->intervals().size();
        }
        return ret;
    }

    inline FieldRangeSet *FieldRangeOrSet::topFrs() const {
        FieldRangeSet *ret = new FieldRangeSet( _baseSet );
        if (_orSets.size()) {
            *ret &= _orSets.front();
        }
        return ret;
    }

    inline FieldRangeSet *FieldRangeOrSet::topFrsOriginal() const {
        FieldRangeSet *ret = new FieldRangeSet( _baseSet );
        if (_originalOrSets.size()) {
            *ret &= _originalOrSets.front();
        }
        return ret;
    }

} // namespace mongo
