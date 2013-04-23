// expression_parser.h

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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"

namespace mongo {

    typedef StatusWith<Expression*> StatusWithExpression;

    class ExpressionParser {
    public:
        static StatusWithExpression parse( const BSONObj& obj );

    private:

        /**
         * parses a field in a sub expression
         * if the query is { x : { $gt : 5, $lt : 8 } }
         * e is { $gt : 5, $lt : 8 }
         */
        static Status _parseSub( const char* name,
                                 const BSONObj& obj,
                                 AndExpression* root );

        /**
         * parses a single field in a sub expression
         * if the query is { x : { $gt : 5, $lt : 8 } }
         * e is $gt : 5
         */
        static StatusWithExpression _parseSubField( const char* name,
                                                    const BSONElement& e );

        static StatusWithExpression _parseComparison( const char* name,
                                                      ComparisonExpression::Type cmp,
                                                      const BSONElement& e );

        static StatusWithExpression _parseMOD( const char* name,
                                               const BSONElement& e );

        static StatusWithExpression _parseRegexElement( const char* name,
                                                        const BSONElement& e );

        static StatusWithExpression _parseRegexDocument( const char* name,
                                                         const BSONObj& doc );


        static Status _parseArrayFilterEntries( ArrayFilterEntries* entries,
                                                const BSONObj& theArray );

        // arrays

        static StatusWithExpression _parseElemMatch( const char* name,
                                                     const BSONElement& e );

        static StatusWithExpression _parseAll( const char* name,
                                               const BSONElement& e );

        // tree

        static Status _parseTreeList( const BSONObj& arr, ListOfExpression* out );

        static StatusWithExpression _parseNot( const char* name, const BSONElement& e );

    };

}
