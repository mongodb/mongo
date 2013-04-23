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

namespace mongo {

    class TreeExpression;
    class MatchDetails;

    class Expression {
        MONGO_DISALLOW_COPYING( Expression );

    public:
        Expression(){}
        virtual ~Expression(){}

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

        virtual string toString() const;
        virtual void debugString( StringBuilder& debug, int level = 0 ) const = 0;
    protected:
        void _debugAddSpace( StringBuilder& debug, int level ) const;
    };

}
