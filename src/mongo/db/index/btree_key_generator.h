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

#include <vector>
#include <set>
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * Internal class used by BtreeAccessMethod to generate keys for indexed documents.
     * This class is meant to be kept under the index access layer.
     */
    class BtreeKeyGenerator {
    public:
        BtreeKeyGenerator(std::vector<const char*> fieldNames, std::vector<BSONElement> fixed, bool isSparse);
        virtual ~BtreeKeyGenerator() { }

        Status getKeys(const BSONObj &obj, BSONObjSet *keys) const;

    protected:
        // These are used by the getKeysImpl(s) below.
        std::vector<const char*> _fieldNames;
        bool _isSparse;
        BSONObj _nullKey; // a full key with all fields null
        BSONObj _nullObj;     // only used for _nullElt
        BSONElement _nullElt; // jstNull
        BSONSizeTracker _sizeTracker;
    private:
        // We have V0 and V1.  Sigh.
        virtual Status getKeysImpl(std::vector<const char*> fieldNames, std::vector<BSONElement> fixed,
                                   const BSONObj &obj, BSONObjSet *keys) const = 0;
        std::vector<BSONElement> _fixed;
    };

    class BtreeKeyGeneratorV0 : public BtreeKeyGenerator {
    public:
        BtreeKeyGeneratorV0(std::vector<const char*> fieldNames, std::vector<BSONElement> fixed,
                            bool isSparse);
        virtual ~BtreeKeyGeneratorV0() { }
        
    private:
        virtual Status getKeysImpl(std::vector<const char*> fieldNames, std::vector<BSONElement> fixed,
                                   const BSONObj &obj, BSONObjSet *keys) const;
    };

    class BtreeKeyGeneratorV1 : public BtreeKeyGenerator {
    public:
        BtreeKeyGeneratorV1(std::vector<const char*> fieldNames, std::vector<BSONElement> fixed,
                            bool isSparse);
        virtual ~BtreeKeyGeneratorV1() { }

    private:
        // Our approach to generating keys is to generate key parts for one field in
        // the key pattern at a time. A 'KeyElementList' stores the elements extracted
        // from the original BSON object for each index field individually. There is one
        // outer list per key pattern field; the inner lists store the extracted elements
        // that will be used to construct the keys.
        //
        // Ex.
        //   Say we have index {a: 1, b: 1} and we are generating keys for the object
        //   {a: 3, b: [4, 5, 6]}. The first step is to construct the following KeyElementList:
        //     [ [ El(3) ], [ El(4), El(5), El(6) ] ]
        //   where the notation El(n) denotes a BSONElement for a numeric field.
        typedef vector< vector<BSONElement> > KeyElementList;

        // Btree key generation is a recursive process. As we recur through the input BSONObj,
        // we pass along a 'KeygenContext', which wraps up any state we need about where we
        // are in the keygen process.
        struct KeygenContext {
            // Index into '_keyPattern' corresponding to the field that we are currently
            // generating fields for. Ex: For key pattern {a: 1, b: 1, c: 1}, 'kpIndex'
            // will be set to 0 when generating keys for "a", 1 when generating keys for "b",
            // and 2 when generating keys for "c".
            size_t kpIndex;

            // Index which keeps track of where we are in the path of the indexed field.
            // Ex: Say we have key pattern {a: 1, "b.c.d": 1, e: 1}, and we are currently
            // generating keys for "b.c.d" (i.e. kpIndex==1). Then 'pathPosition' will be 0
            // when generating keys for "b", 1 when generating keys for "b.c", and 2 when
            // generating keys for "b.c.d".
            size_t pathPosition;

            // Are we generating keys for an element of an indexed array?
            bool inArray;
        };

        /**
         * Top-level key generation implementation for V1 btree indices. Parameters:
         *   'fieldNames' -- A list of the field names in the index key pattern. For index
         *      {"a.b.c": 1, "foo": -1, "bar.0.a": 1}, then this will be ["a.b.c", "foo", "bar.0.a"]
         *   'fixed' -- Required by the method signature in the superclass, but not used.
         *   'obj' -- The object for which we will generate index keys.
         *
         * Returns the set of index keys generated from 'obj' according to the key pattern
         * in 'fieldNames' through the out-parameter 'keys'.
         */
        virtual Status getKeysImpl(std::vector<const char*> fieldNames, std::vector<BSONElement> fixed,
                                   const BSONObj &obj, BSONObjSet *keys) const;

        /**
         * Generates keys for the BSONElement 'el' according to the context in 'context' and
         * the index key pattern stored in '_keyPattern'.
         */
        Status getKeysForElement(const BSONElement& el,
                                 KeygenContext context,
                                 std::vector<BSONElement>* out) const;

        /**
         * getKeysForElement(...) delegates to this function if 'el' is an indexed array. Special
         * key generation logic for arrays lives here.
         */
        Status getKeysForArrayElement(const BSONElement& el,
                                      KeygenContext context,
                                      std::vector<BSONElement>* out) const;

        /**
         * A helper for getKeysForArrayElement(...) which handles key patterns that have a
         * positional element, such as {"a.0": 1}.
         *
         * Returns true through the out-parameter 'isPositional' if keys have been successively
         * generated for a positional pattern. Otherwise sets 'isPositional' to false (returning
         * a status of OK) and does nothing.
         */
        Status handlePositionalKeypattern(const BSONElement& el,
                                          KeygenContext context,
                                          bool* isPositional,
                                          std::vector<BSONElement>* out) const;

        /**
         * getKeysForElement(...) delegates to this function if 'el' is neither an indexed
         * array nor an indexed subobject.
         */
        Status getKeysForSimpleElement(const BSONElement& el,
                                       KeygenContext context,
                                       std::vector<BSONElement>* out) const;

        /**
         * Outputs the final btree keys in 'keys', constructing them using the extracted
         * BSONElements in 'kel'.
         *
         * Ex.
         *   Continuing the example from above, suppose that 'kel' is as show below:
         *     [ [ El(3) ], [ El(4), El(5), El(6) ] ]
         *   We would return the following three keys:
         *     {"": 3, "": 4}
         *     {"": 3, "": 5}
         *     {"": 3, "": 6}
         */
        void buildKeyObjects(const KeyElementList& kel, BSONObjSet *keys) const;

        //
        // Helper functions.
        //

        /**
         * Returns an error status if 'obj' has multiple arrays at position 'pathPosition' that
         * are indexed. This is made illegal because it would require generating keys for the
         * Cartesian product.
         *
         * Ex:
         *   Say we have key pattern {"a.b": 1, "a.c": 1} and 'obj' is {a: {b: [1], c: [2]}}.
         *   This method will return an error when 'pathPosition' is 1, as both "a.b" and "a.c"
         *   are arrays.
         */
        Status checkForParallelArrays(const BSONObj& obj, size_t pathPosition) const;

        /**
         * Returns true if 'context' points to the last position in the path for the
         * current field.
         *
         * Ex.
         *   Say the keypattern is {"a": 1, "b.c.d": 1, "e": 1} and 'context.kpIndex'
         *   is 1 (we are generating keys for "b.c.d"). This function will return true
         *   if and only if 'context.pathPosition' is 2.
         */
        bool lastPathPosition(const KeygenContext& context) const;

        //
        // Member vars.
        //

        // A representation of the key pattern as a list of list of strings. There is one outer
        // list per field in the index. Each of these fields is represented as a lit of the dotted
        // path components.
        //
        // Ex.
        //  Say the index is {"a.b.c": 1, "foo": -1, "bar.0.a": 1}. Then '_keyPattern' is:
        //    [ [ "a", "b", "c" ],
        //      [ "foo" ],
        //      [ "bar", "0", "a" ] ]
        typedef std::vector< std::vector<string> > KeyPattern;
        KeyPattern _keyPattern;

        // The BSONObj {"": undefined}.
        BSONObj _undefinedObj;

        // A BSONElement containing value 'undefined'.
        BSONElement _undefinedElt;
    };

}  // namespace mongo
