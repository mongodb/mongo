// @file queryoptimizercursorimpl.h

/**
 *    Copyright (C) 2011 10gen Inc.
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

#include "queryoptimizercursor.h"

namespace mongo {
    
    /** Helper class for caching and counting matches during execution of a QueryPlan. */
    class CachedMatchCounter {
    public:
        /**
         * @param aggregateNscanned - shared count of nscanned for this and othe plans.
         * @param cumulativeCount - starting point for accumulated count over a series of plans.
         */
        CachedMatchCounter( long long &aggregateNscanned, int cumulativeCount ) : _aggregateNscanned( aggregateNscanned ), _nscanned(), _cumulativeCount( cumulativeCount ), _count(), _checkDups(), _match( Unknown ), _counted() {}
        
        /** Set whether dup checking is enabled when counting. */
        void setCheckDups( bool checkDups ) { _checkDups = checkDups; }
        
        /**
         * Usual sequence of events:
         * 1) resetMatch() - reset stored match value to Unkonwn.
         * 2) setMatch() - set match value to a definite true/false value.
         * 3) knowMatch() - check if setMatch() has been called.
         * 4) countMatch() - increment count if match is true.
         */
        
        void resetMatch() {
            _match = Unknown;
            _counted = false;
        }
        /** @return true if the match was not previously recorded. */
        bool setMatch( bool match ) {
            MatchState oldMatch = _match;
            _match = match ? True : False;
            return _match == True && oldMatch != True;
        }
        bool knowMatch() const { return _match != Unknown; }
        void countMatch( const DiskLoc &loc ) {
            if ( !_counted && _match == True && !getsetdup( loc ) ) {
                ++_cumulativeCount;
                ++_count;
                _counted = true;
            }
        }
        bool wouldCountMatch( const DiskLoc &loc ) const {
            return !_counted && _match == True && !getdup( loc );
        }

        bool enoughCumulativeMatchesToChooseAPlan() const {
            // This is equivalent to the default condition for switching from
            // a query to a getMore, which was the historical default match count for
            // choosing a plan.
            return _cumulativeCount >= 101;
        }
        bool enoughMatchesToRecordPlan() const {
            // Recording after 50 matches is a historical default (101 default limit / 2).
            return _count > 50;
        }

        int cumulativeCount() const { return _cumulativeCount; }
        int count() const { return _count; }
        
        /** Update local and aggregate nscanned counts. */
        void updateNscanned( long long nscanned ) {
            _aggregateNscanned += ( nscanned - _nscanned );
            _nscanned = nscanned;
        }
        long long nscanned() const { return _nscanned; }
        long long &aggregateNscanned() const { return _aggregateNscanned; }
    private:
        bool getsetdup( const DiskLoc &loc ) {
            if ( !_checkDups ) {
                return false;
            }
            pair<set<DiskLoc>::iterator, bool> p = _dups.insert( loc );
            return !p.second;
        }
        bool getdup( const DiskLoc &loc ) const {
            if ( !_checkDups ) {
                return false;
            }
            return _dups.find( loc ) != _dups.end();
        }
        long long &_aggregateNscanned;
        long long _nscanned;
        int _cumulativeCount;
        int _count;
        bool _checkDups;
        enum MatchState { Unknown, False, True };
        MatchState _match;
        bool _counted;
        set<DiskLoc> _dups;
    };
    
} // namespace mongo
