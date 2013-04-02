// XXX THIS FILE IS DEPRECATED.  PLEASE DON'T MODIFY WITHOUT TALKING TO HK

// hashindex.h

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

#include "mongo/db/hasher.h"
#include "mongo/db/index.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher.h"
#include "mongo/db/namespace-inl.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    /* This is an index where the keys are hashes of a given field.
     *
     * Optional arguments:
     *  "seed" : int (default = 0, a seed for the hash function)
     *  "hashVersion : int (default = 0, determines which hash function to use)
     *
     * Example use in the mongo shell:
     * > db.foo.ensureIndex({a : "hashed"}, {seed : 3, hashVersion : 0})
     *
     * LIMITATION: Only works with a single field. The HashedIndexType
     * constructor uses uassert to ensure that the spec has the form
     * {<fieldname> : "hashed"}, and not, for example,
     * { a : "hashed" , b : 1}
     *
     * LIMITATION: Cannot be used as a unique index.
     * The HashedIndexType constructor uses uassert to ensure that
     * the spec does not contain {"unique" : true}
     *
     * LIMITATION: Cannot be used to index arrays.
     * The getKeys function uasserts that value being inserted
     * is not an array.  This index will not be built if any
     * array values of the hashed field exist.
     *
     */
    class HashedIndexType : public IndexType{
    public:

        static const string HASHED_INDEX_TYPE_IDENTIFIER;
        typedef int HashVersion;

        /* Creates a new HashedIndex around a HashedIndexPlugin
         * and an IndexSpec. New HashedIndexTypes are created via
         * a factory method in the HashedIndexPlugin class.
         */
        HashedIndexType( const IndexPlugin* plugin , const IndexSpec* spec );
        virtual ~HashedIndexType();

        /* This index is only considered "HELPFUL" for a query
         * if it's the union of at least one equality constraint on the
         * hashed field.  Otherwise it's considered USELESS.
         * Example queries (supposing the indexKey is {a : "hashed"}):
         *   {a : 3}  HELPFUL
         *   {a : 3 , b : 3} HELPFUL
         *   {a : {$in : [3,4]}} HELPFUL
         *   {a : {$gte : 3, $lte : 3}} HELPFUL
         *   {} USELESS
         *   {b : 3} USELESS
         *   {a : {$gt : 3}} USELESS
         */
        IndexSuitability suitability( const FieldRangeSet& queryConstraints ,
                                      const BSONObj& order ) const;

        /* The input is "obj" which should have a field corresponding to the hashedfield.
         * The output is a BSONObj with a single BSONElement whose value is the hash
         * Eg if this is an index on "a" we have
         *   obj is {a : 45} --> key becomes {"" : hash(45) }
         *
         * Limitation: arrays values are not currently supported.  This function uasserts
         * that the value is not an array, and errors out in that case.
         */
        void getKeys( const BSONObj &obj, BSONObjSet &keys ) const;

        /* A field missing from a document is represented by the hash value of a null BSONElement.
         */
        BSONElement missingField() const { return _missingKey.firstElement(); }

        /* The newCursor method works for suitable queries by generating a BtreeCursor
         * using the hash of point-intervals parsed by FieldRangeSet.
         * For unsuitable queries it just instantiates a btree cursor over the whole tree
         */
        shared_ptr<Cursor> newCursor( const BSONObj& query ,
                const BSONObj& order , int numWanted ) const;

        /* Takes a BSONElement, seed and hashVersion, and outputs the
         * 64-bit hash used for this index
         * E.g. if the element is {a : 3} this outputs v1-hash(3)
         * */
        static long long int makeSingleKey( const BSONElement& e ,
                                            HashSeed seed ,
                                            HashVersion v = 0 );

        /* Since the keys for this index are hashes, documents are not stored in order,
         * thus we will need to perform scanAndOrder whenever the "order" is non-empty.
         */
        bool scanAndOrderRequired( const BSONObj& query , const BSONObj& order ) const {
            return ! order.isEmpty();
        }

    private:
        string _hashedField;
        KeyPattern _keyPattern;
        HashSeed _seed; //defaults to zero if not in the IndexSpec
        HashVersion _hashVersion; //defaults to zero if not in the IndexSpec
        bool _isSparse;
        BSONObj _missingKey;
    };

}
