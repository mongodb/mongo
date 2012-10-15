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
*/

#pragma once

#include "mongo/db/jsobj.h"

namespace mongo {

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
        KeyPattern( const BSONObj& pattern ): _pattern( pattern ) {}

        /*
         *  Returns a BSON representation of this KeyPattern.
         */
        BSONObj toBSON() const { return _pattern; }

        /*
         * Returns true if the given fieldname is the name of one element of the (potentially)
         * compound key described by this KeyPattern.
         */
        bool hasField( const char* fieldname ) const { return _pattern.hasField( fieldname ); }

        /*
         * Returns true if the key described by this KeyPattern is a prefix of
         * the (potentially) compound key described by 'other'
         */
        bool isPrefixOf( const KeyPattern& other ) const {
            return _pattern.isPrefixOf( other.toBSON() );
        }

        /**
         * Returns true if this KeyPattern contains any computed values, (e.g. {a : "hashed"}),
         * and false if this KeyPattern consists of only ascending/descending fields
         * (e.g. {a : 1, b : -1}). With our current index expression language, "special" patterns
         * are any patterns that are not a simple list of field names and 1/-1 values.
         */
        bool isSpecial() const;

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

        /**@param fromQuery a FieldRangeSet formed from parsing a query
         * @return an ordered list of bounds generated using this KeyPattern
         * and the constraints from the FieldRangeSet
         *
         * The value of frsp->matchPossibleForSingleKeyFRS(fromQuery) should be true,
         * otherwise this function could throw.
         *
         */
        BoundList keyBounds( const FieldRangeSet& queryConstraints ) const;

    private:
        BSONObj _pattern;
    };


} // namespace mongo
