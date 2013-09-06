// matcher_covered.h

/* Matcher is our boolean expression evaluator for "where" clauses */

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

#include "mongo/db/diskloc.h"
#include "mongo/db/geo/geoquery.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"
#include "mongo/db/matcher/match_details.h"

namespace mongo {

    // If match succeeds on index key, then attempt to match full document.
    class CoveredIndexMatcher : boost::noncopyable {
    public:
        CoveredIndexMatcher(const BSONObj &pattern, const BSONObj &indexKeyPattern);
        bool matchesWithSingleKeyIndex( const BSONObj& key, const DiskLoc& recLoc,
                                        MatchDetails* details = 0 ) const {
            return matches( key, recLoc, details, true );
        }
        /**
         * This is the preferred method for matching against a cursor, as it
         * can handle both multi and single key cursors.
         */
        bool matchesCurrent( Cursor * cursor , MatchDetails * details = 0 ) const;
        bool needRecord() const { return _needRecord; }

        const Matcher &docMatcher() const { return *_docMatcher; }

        /**
         * @return a matcher for a following $or clause.
         * @param prevClauseFrs The index range scanned by the previous $or clause.  May be empty.
         * @param nextClauseIndexKeyPattern The index key of the following $or clause.
         */
        CoveredIndexMatcher *nextClauseMatcher( const shared_ptr<FieldRangeVector>& prevClauseFrv,
                                                const BSONObj& nextClauseIndexKeyPattern ) const {
            return new CoveredIndexMatcher( *this, prevClauseFrv, nextClauseIndexKeyPattern );
        }

        string toString() const;

    private:
        bool matches( const BSONObj& key, const DiskLoc& recLoc, MatchDetails* details = 0,
                      bool keyUsable = true ) const;
        bool isOrClauseDup( const BSONObj &obj ) const;
        CoveredIndexMatcher( const CoveredIndexMatcher &prevClauseMatcher,
                            const shared_ptr<FieldRangeVector> &prevClauseFrv,
                            const BSONObj &nextClauseIndexKeyPattern );
        void init();
        shared_ptr< Matcher > _docMatcher;
        Matcher _keyMatcher;
        vector<shared_ptr<FieldRangeVector> > _orDedupConstraints;

        bool _needRecord; // if the key itself isn't good enough to determine a positive match
    };

} // namespace mongo

