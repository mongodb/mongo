// @file keypattern.h - Utilities for manipulating index/shard key patterns.

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

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/unordered_set.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    struct FieldInterval;
    class FieldRangeSet;

    /**
     * A BoundList contains intervals specified by inclusive start
     * and end bounds.  The intervals should be nonoverlapping and occur in
     * the specified direction of traversal.  For example, given a simple index {i:1}
     * and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
     * would be valid for index {i:-1} with direction -1.
     */
    typedef vector<pair<BSONObj,BSONObj> > BoundList;

    /** A KeyPattern is an expression describing a transformation of a document into a
     *  document key.  Document keys are used to store documents in indices and to target
     *  sharded queries.
     *
     *  Examples:
     *    { a : 1 }
     *    { a : 1 , b  : -1 }
     *    { a : "hashed" }
     */
    class KeyPattern {
    public:
        /*
         * We are allowing implicit conversion from BSON
         */
        KeyPattern( const BSONObj& pattern );

        /*
         *  Returns a BSON representation of this KeyPattern.
         */
        BSONObj toBSON() const { return _pattern; }

        /*
         * Returns true if the given fieldname is the (dotted prefix of the) name of one
         * element of the (potentially) compound key described by this KeyPattern.
         */
        bool hasField( const StringData& fieldname ) const {
            return _prefixes.find( fieldname ) != _prefixes.end();
        }

        /*
         * Gets the element of this pattern corresponding to the given fieldname.
         * Returns eoo if none exists.
         */
        BSONElement getField( const char* fieldname ) const { return _pattern[ fieldname ]; }

        /*
         * Returns true if the key described by this KeyPattern is a prefix of
         * the (potentially) compound key described by 'other'
         */
        bool isPrefixOf( const KeyPattern& other ) const {
            return _pattern.isPrefixOf( other.toBSON() );
        }

        /**
         * Is the provided key pattern the index over the ID field?
         * The always required ID index is always {_id: 1} or {_id: -1}.
         */
        static bool isIdKeyPattern(const BSONObj& pattern);

        /* Takes a BSONObj whose field names are a prefix of the fields in this keyPattern, and
         * outputs a new bound with MinKey values appended to match the fields in this keyPattern
         * (or MaxKey values for descending -1 fields). This is useful in sharding for
         * calculating chunk boundaries when tag ranges are specified on a prefix of the actual
         * shard key, or for calculating index bounds when the shard key is a prefix of the actual
         * index used.
         *
         * @param makeUpperInclusive If true, then MaxKeys instead of MinKeys will be appended, so
         * that the output bound will compare *greater* than the bound being extended (note that
         * -1's in the keyPattern will swap MinKey/MaxKey vals. See examples).
         *
         * Examples:
         * If this keyPattern is {a : 1}
         *   extendRangeBound( {a : 55}, false) --> {a : 55}
         *
         * If this keyPattern is {a : 1, b : 1}
         *   extendRangeBound( {a : 55}, false) --> {a : 55, b : MinKey}
         *   extendRangeBound( {a : 55}, true ) --> {a : 55, b : MaxKey}
         *
         * If this keyPattern is {a : 1, b : -1}
         *   extendRangeBound( {a : 55}, false) --> {a : 55, b : MaxKey}
         *   extendRangeBound( {a : 55}, true ) --> {a : 55, b : MinKey}
         */
        BSONObj extendRangeBound( const BSONObj& bound , bool makeUpperInclusive ) const;

        /**
         * Returns true if this KeyPattern contains any computed values, (e.g. {a : "hashed"}),
         * and false if this KeyPattern consists of only ascending/descending fields
         * (e.g. {a : 1, b : -1}). With our current index expression language, "special" patterns
         * are any patterns that are not a simple list of field names and 1/-1 values.
         */
        bool isSpecial() const;

        /**
         * Returns true if the quantities stored in this KeyPattern can be used to compute all the
         * quantities in "other". Useful for determining whether an index based on one KeyPattern
         * can be used as a covered index for a query based on another.
         */
        bool isCoveredBy( const KeyPattern& other ) const;

        string toString() const{ return toBSON().toString(); }

        /* Given a document, extracts the index key corresponding to this KeyPattern
         * Warning: assumes that there is a *single* key to be extracted!
         *
         * Examples:
         *  If 'this' KeyPattern is { a  : 1 }
         *   { a: "hi" , b : 4} --> returns { a : "hi" }
         *   { c : 4 , a : 2 } -->  returns { a : 2 }
         *   { b : 2 }  (bad input, don't call with this)
         *   { a : [1,2] }  (bad input, don't call with this)
         *
         *  If 'this' KeyPattern is { a  : "hashed" }
         *   { a: 1 } --> returns { a : NumberLong("5902408780260971510")  }
         */
        BSONObj extractSingleKey( const BSONObj& doc ) const;

        /**@param queryConstraints a FieldRangeSet, usually formed from parsing a query
         * @return an ordered list of bounds generated using this KeyPattern and the
         * constraints from the FieldRangeSet.  This function is used in sharding to
         * determine where to route queries according to the shard key pattern.
         *
         * Examples:
         * If this KeyPattern is { a : 1  }
         * FieldRangeSet( {a : 5 } ) --> returns [{a : 5}, {a : 5 } ]
         * FieldRangeSet( {a : {$gt : 3}} ) --> returns [({a : 3} , { a : MaxInt})]
         *
         * If this KeyPattern is { a : "hashed }
         * FieldRangeSet( {a : 5 } --> returns [ ({ a : hash(5) }, {a : hash(5) }) ]
         *
         * The bounds returned by this function may be a superset of those defined
         * by the constraints.  For instance, if this KeyPattern is {a : 1}
         * FieldRanget( { a : {$in : [1,2]} , b : {$in : [3,4,5]} } )
         *        --> returns [({a : 1 , b : 3} , {a : 1 , b : 5}]),
         *                    [({a : 2 , b : 3} , {a : 2 , b : 5}])
         *
         * The queryConstraints should be defined for all the fields in this keypattern
         * (i.e. the value of frsp->matchPossibleForSingleKeyFRS(_pattern) should be true,
         * otherwise this function could throw).
         *
         */
        BoundList keyBounds( const FieldRangeSet& queryConstraints ) const;

    private:
        BSONObj _pattern;

        // Each field in the '_pattern' may be itself a dotted field. We store all the prefixes
        // of each field here. For instance, if a pattern is { 'a.b.c': 1, x: 1 }, we'll store
        // here 'a', 'a.b', 'a.b.c', and 'x'.
        //
        // Since we're indexing into '_pattern's field names, it must stay constant after
        // constructed.
        struct PrefixHasher {
            size_t operator()( const StringData& strData ) const {
                size_t result = 0;
                const char* p = strData.rawData();
                for (size_t len = strData.size(); len > 0; len-- ) {
                    result = ( result * 131 ) + *p++;
                }
                return result;
            }
        };
        unordered_set<StringData, PrefixHasher> _prefixes;

        bool isAscending( const BSONElement& fieldExpression ) const {
            return ( fieldExpression.isNumber()  && fieldExpression.numberInt() == 1 );
        }

        bool isDescending( const BSONElement& fieldExpression ) const {
            return ( fieldExpression.isNumber()  && fieldExpression.numberInt() == -1 );
        }

        bool isHashed( const BSONElement& fieldExpression ) const {
            return mongoutils::str::equals( fieldExpression.valuestrsafe() , "hashed" );
        }

        /* Takes a list of intervals corresponding to constraints on a given field
         * in this keypattern, and transforms them into a list of bounds
         * based on the expression for 'field'
         */
        BoundList _transformFieldBounds( const vector<FieldInterval>& oldIntervals ,
                                         const BSONElement& field ) const;

    };

} // namespace mongo
