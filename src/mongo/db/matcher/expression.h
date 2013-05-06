// expression.h

/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/matcher/match_details.h"

namespace mongo {

    class TreeMatchExpression;

    class MatchExpression {
        MONGO_DISALLOW_COPYING( MatchExpression );
    public:
        enum MatchType {
            // tree types
            AND, OR, NOR, NOT,

            // array types
            ALL, ELEM_MATCH_OBJECT, ELEM_MATCH_VALUE, SIZE,

            // leaf types
            LTE, LT, EQ, GT, GTE, NE, REGEX, MOD, EXISTS, IN, NIN,

            // special types
            TYPE_OPERATOR, GEO, WHERE,

            // things that maybe shouldn't even be nodes
            ATOMIC
        };

        enum MatchCategory {
            LEAF, ARRAY, TREE, TYPE_CATEGORY, SPECIAL
        };

        MatchExpression( MatchCategory category, MatchType type );
        virtual ~MatchExpression(){}

        /**
         * determins if the doc matches the expression
         * there could be an expression that looks at fields, or the entire doc
         */
        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const = 0;

        /**
         * does the element match the expression
         * not valid for all expressions ($where) where this will immediately return false
         */
        virtual bool matchesSingleElement( const BSONElement& e ) const = 0;

        virtual size_t numChildren() const { return 0; }
        virtual const MatchExpression* getChild( size_t i ) const { return NULL; }

        MatchType matchType() const { return _matchType; }
        MatchCategory matchCategory() const { return _matchCategory; }

        virtual string toString() const;
        virtual void debugString( StringBuilder& debug, int level = 0 ) const = 0;

        virtual bool equivalent( const MatchExpression* other ) const = 0;
    protected:
        void _debugAddSpace( StringBuilder& debug, int level ) const;

    private:
        MatchCategory _matchCategory;
        MatchType _matchType;
    };

    /**
     * this isn't really an expression, but a hint to other things
     * not sure where to put it in the end
     */
    class AtomicMatchExpression : public MatchExpression {
    public:
        AtomicMatchExpression() : MatchExpression( SPECIAL, ATOMIC ){}

        virtual bool matches( const BSONObj& doc, MatchDetails* details = 0 ) const {
            return true;
        }

        virtual bool matchesSingleElement( const BSONElement& e ) const {
            return true;
        }

        virtual void debugString( StringBuilder& debug, int level = 0 ) const;

        virtual bool equivalent( const MatchExpression* other ) const {
            return other->matchType() == ATOMIC;
        }

    };
}
