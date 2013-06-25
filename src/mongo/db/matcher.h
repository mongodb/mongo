// matcher.h

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
*/

#pragma once

#include "mongo/db/jsobj.h"

namespace mongo {

    class Cursor;
    class CoveredIndexMatcher;
    class FieldRangeVector;

    struct element_lt {
        bool operator()(const BSONElement& l, const BSONElement& r) const {
            int x = (int) l.canonicalType() - (int) r.canonicalType();
            if ( x < 0 ) return true;
            else if ( x > 0 ) return false;
            return compareElementValues(l,r) < 0;
        }
    };
}

#include "mongo/db/matcher/matcher.h"

namespace mongo {

    //typedef MatcherOld Matcher;
    typedef Matcher2 Matcher;
}

