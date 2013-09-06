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

#include <boost/function.hpp>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"

namespace mongo {

    typedef StatusWith<MatchExpression*> StatusWithMatchExpression;

    class MatchExpressionParser {
    public:

        /**
         * caller has to maintain ownership obj
         * the tree has views (BSONElement) into obj
         */
        static StatusWithMatchExpression parse( const BSONObj& obj ) {
            return _parse( obj, true );
        }

    private:

        /**
         * 5 = false
         { a : 5 } = false
         { $lt : 5 } = true
         { $ref : "s" } = false
         */
        static bool _isExpressionDocument( const BSONElement& e );

        static StatusWithMatchExpression _parse( const BSONObj& obj, bool topLevel );

        /**
         * parses a field in a sub expression
         * if the query is { x : { $gt : 5, $lt : 8 } }
         * e is { $gt : 5, $lt : 8 }
         */
        static Status _parseSub( const char* name,
                                 const BSONObj& obj,
                                 AndMatchExpression* root );

        /**
         * parses a single field in a sub expression
         * if the query is { x : { $gt : 5, $lt : 8 } }
         * e is $gt : 5
         */
        static StatusWithMatchExpression _parseSubField( const BSONObj& context,
                                                         const AndMatchExpression* andSoFar,
                                                         const char* name,
                                                         const BSONElement& e );

        static StatusWithMatchExpression _parseComparison( const char* name,
                                                           ComparisonMatchExpression* cmp,
                                                           const BSONElement& e );

        static StatusWithMatchExpression _parseMOD( const char* name,
                                               const BSONElement& e );

        static StatusWithMatchExpression _parseRegexElement( const char* name,
                                                        const BSONElement& e );

        static StatusWithMatchExpression _parseRegexDocument( const char* name,
                                                         const BSONObj& doc );


        static Status _parseArrayFilterEntries( ArrayFilterEntries* entries,
                                                const BSONObj& theArray );

        // arrays

        static StatusWithMatchExpression _parseElemMatch( const char* name,
                                                     const BSONElement& e );

        static StatusWithMatchExpression _parseAll( const char* name,
                                               const BSONElement& e );

        // tree

        static Status _parseTreeList( const BSONObj& arr, ListOfMatchExpression* out );

        static StatusWithMatchExpression _parseNot( const char* name, const BSONElement& e );

    };

    typedef boost::function<StatusWithMatchExpression(const char* name, int type, const BSONObj& section)> MatchExpressionParserGeoCallback;
    extern MatchExpressionParserGeoCallback expressionParserGeoCallback;

    typedef boost::function<StatusWithMatchExpression(const BSONElement& where)> MatchExpressionParserWhereCallback;
    extern MatchExpressionParserWhereCallback expressionParserWhereCallback;

}
