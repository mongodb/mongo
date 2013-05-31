// fts_search.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#include <map>
#include <set>
#include <vector>
#include <queue>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/fts/fts_matcher.h"
#include "mongo/db/fts/fts_query.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher.h"

namespace mongo {

    class BtreeCursor;

    namespace fts {

        // priority queue template, for use when we're populating results
        // vector returned to the user. extends the default priority_queue
        // by providing direct access to the underlying vector, which should
        // be used CAREFULLY because you can get into trouble..
        template <class T, class S, class C>
        class a_priority_queue : public std::priority_queue<T, S, C> {
        public:
            // return the value of an element at position n when we call pq[n]
            T operator[](const int &n) { return this->c[n]; }
            // return underlying data structure. called dangerous because it is.
            S dangerous() { return this->c; }
        };

        typedef a_priority_queue<ScoredLocation, vector<ScoredLocation>, ScoredLocationComp> Results;

        class FTSSearch {
            MONGO_DISALLOW_COPYING(FTSSearch);
        public:

            typedef std::map<Record*,double> Scores;

            FTSSearch( IndexDescriptor* descriptor,
                       const FTSSpec& ftsSpec,
                       const BSONObj& indexPrefix,
                       const FTSQuery& query,
                       const BSONObj& filter );

            void go(Results* results, unsigned limit );

            long long getKeysLookedAt() const { return _keysLookedAt; }
            long long getObjLookedAt() const { return _objectsLookedAt; }

        private:

            void _process( BtreeCursor* cursor );

            /**
             * checks not index pieces
             * i.e. prhases & negated terms
             */
            bool _ok( Record* record ) const;

            IndexDescriptor* _descriptor;
            const FTSSpec& _ftsSpec;
            BSONObj _indexPrefix;
            FTSQuery _query;
            FTSMatcher _ftsMatcher;

            scoped_ptr<CoveredIndexMatcher> _matcher;

            long long _keysLookedAt;
            long long _objectsLookedAt;

            Scores _scores;

        };

    } // namespace fts

} // namespace mongo

