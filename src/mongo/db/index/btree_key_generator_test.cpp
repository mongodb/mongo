/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/index/btree_key_generator.h"

#include "mongo/db/json.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    //
    // Helper functions
    //

    std::string dumpKeyset(const BSONObjSet& objs) {
        std::stringstream ss;
        ss << "[ ";
        for (BSONObjSet::iterator i = objs.begin(); i != objs.end(); ++i) {
            ss << i->toString() << " ";
        }
        ss << "]";

        return ss.str();
    }

    bool keysetsMatch(const BSONObjSet& expected, const BSONObjSet& actual) {
        if (expected.size() != actual.size()) {
            return false;
        }

        for (BSONObjSet::iterator i = expected.begin(); i != expected.end(); ++i) {
            if (actual.end() == actual.find(*i)) {
                return false;
            }
        }

        return true;
    }

    bool testKeygen(const BSONObj& kp, const BSONObj& obj,
                    const BSONObjSet& expectedKeys, bool sparse = false) {
        //
        // Step 1: construct the btree key generator object, using the
        // index key pattern.
        //
        vector<const char*> fieldNames;
        vector<BSONElement> fixed;

        BSONObjIterator it(kp);
        while (it.more()) {
            BSONElement elt = it.next();
            fieldNames.push_back(elt.fieldName());
            fixed.push_back(BSONElement());
        }

        scoped_ptr<BtreeKeyGenerator> keyGen(
            new BtreeKeyGeneratorV1(fieldNames, fixed, sparse));

        //
        // Step 2: ask 'keyGen' to generate index keys for the object 'obj'.
        //
        BSONObjSet actualKeys;
        keyGen->getKeys(obj, &actualKeys);

        //
        // Step 3: check that the results match the expected result.
        //
        bool match = keysetsMatch(expectedKeys, actualKeys);
        if (!match) {
            cout << "Expected: " << dumpKeyset(expectedKeys) << ", "
                 << "Actual: " << dumpKeyset(actualKeys) << endl;
        }

        return match;
    }

    //
    // Unit tests
    //

    TEST(BtreeKeyGeneratorTest, GetKeysFromObjectSimple) {
        BSONObj keyPattern = fromjson("{a: 1}");
        BSONObj genKeysFrom = fromjson("{b: 4, a: 5}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 5}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromObjectDotted) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a: {b: 4}, c: 'foo'}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 4}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromArraySimple) {
        BSONObj keyPattern = fromjson("{a: 1}");
        BSONObj genKeysFrom = fromjson("{a: [1, 2, 3]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        expectedKeys.insert(fromjson("{'': 2}"));
        expectedKeys.insert(fromjson("{'': 3}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromArrayFirstElement) {
        BSONObj keyPattern = fromjson("{a: 1, b: 1}");
        BSONObj genKeysFrom = fromjson("{a: [1, 2, 3], b: 2}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1, '': 2}"));
        expectedKeys.insert(fromjson("{'': 2, '': 2}"));
        expectedKeys.insert(fromjson("{'': 3, '': 2}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromArraySecondElement) {
        BSONObj keyPattern = fromjson("{first: 1, a: 1}");
        BSONObj genKeysFrom = fromjson("{first: 5, a: [1, 2, 3]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 5, '': 1}"));
        expectedKeys.insert(fromjson("{'': 5, '': 2}"));
        expectedKeys.insert(fromjson("{'': 5, '': 3}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromSecondLevelArray) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a: {b: [1, 2, 3]}}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        expectedKeys.insert(fromjson("{'': 2}"));
        expectedKeys.insert(fromjson("{'': 3}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromParallelArraysBasic) {
        BSONObj keyPattern = fromjson("{'a': 1, 'b': 1}");
        BSONObj genKeysFrom = fromjson("{a: [1, 2, 3], b: [1, 2, 3]}}");
        BSONObjSet expectedKeys;
        ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys), UserException);
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromArraySubobjectBasic) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a: [{b:1,c:4}, {b:2,c:4}, {b:3,c:4}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        expectedKeys.insert(fromjson("{'': 2}"));
        expectedKeys.insert(fromjson("{'': 3}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysArraySubobjectCompoundIndex) {
        BSONObj keyPattern = fromjson("{'a.b': 1, d: 99}");
        BSONObj genKeysFrom = fromjson("{a: [{b:1,c:4}, {b:2,c:4}, {b:3,c:4}], d: 99}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1, '': 99}"));
        expectedKeys.insert(fromjson("{'': 2, '': 99}"));
        expectedKeys.insert(fromjson("{'': 3, '': 99}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysArraySubobjectSingleMissing) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a: [{foo: 41}, {b:1,c:4}, {b:2,c:4}, {b:3,c:4}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null}"));
        expectedKeys.insert(fromjson("{'': 1}"));
        expectedKeys.insert(fromjson("{'': 2}"));
        expectedKeys.insert(fromjson("{'': 3}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromArraySubobjectMissing) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a: [{foo: 41}, {foo: 41}, {foo: 41}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysMissingField) {
        BSONObj keyPattern = fromjson("{a: 1}");
        BSONObj genKeysFrom = fromjson("{b: 1}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysSubobjectMissing) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a: [1, 2]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromCompound) {
        BSONObj keyPattern = fromjson("{x: 1, y: 1}");
        BSONObj genKeysFrom = fromjson("{x: 'a', y: 'b'}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 'a', '': 'b'}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromCompoundMissing) {
        BSONObj keyPattern = fromjson("{x: 1, y: 1}");
        BSONObj genKeysFrom = fromjson("{x: 'a'}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 'a', '': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromArraySubelementComplex) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:[{b:[2]}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 2}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromParallelArraysComplex) {
        BSONObj keyPattern = fromjson("{'a.b': 1, 'a.c': 1}");
        BSONObj genKeysFrom = fromjson("{a:[{b:[1],c:[2]}]}");
        BSONObjSet expectedKeys;
        ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys), UserException);
    }

    TEST(BtreeKeyGeneratorTest, GetKeysAlternateMissing) {
        BSONObj keyPattern = fromjson("{'a.b': 1, 'a.c': 1}");
        BSONObj genKeysFrom = fromjson("{a:[{b:1},{c:2}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null, '': 2}"));
        expectedKeys.insert(fromjson("{'': 1, '': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromMultiComplex) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:[{b:1},{b:[1,2,3]}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        expectedKeys.insert(fromjson("{'': 2}"));
        expectedKeys.insert(fromjson("{'': 3}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysArrayEmpty) {
        BSONObj keyPattern = fromjson("{a: 1}");
        BSONObj genKeysFrom = fromjson("{a:[1,2]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        expectedKeys.insert(fromjson("{'': 2}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a: [1]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a: null}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a: []}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromDoubleArray) {
        BSONObj keyPattern = fromjson("{a: 1, a: 1}");
        BSONObj genKeysFrom = fromjson("{a:[1,2]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1, '': 1}"));
        expectedKeys.insert(fromjson("{'': 2, '': 2}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromDoubleEmptyArray) {
        BSONObj keyPattern = fromjson("{a: 1, a: 1}");
        BSONObj genKeysFrom = fromjson("{a:[]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': undefined, '': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromMultiEmptyArray) {
        BSONObj keyPattern = fromjson("{a: 1, b: 1}");
        BSONObj genKeysFrom = fromjson("{a: 1, b: [1, 2]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1, '': 1}"));
        expectedKeys.insert(fromjson("{'': 1, '': 2}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a: 1, b: [1]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1, '': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a: 1, b: []}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1, '': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromNestedEmptyArray) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:[]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromMultiNestedEmptyArray) {
        BSONObj keyPattern = fromjson("{'a.b': 1, 'a.c': 1}");
        BSONObj genKeysFrom = fromjson("{a:[]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null, '': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromUnevenNestedEmptyArray) {
        BSONObj keyPattern = fromjson("{'a': 1, 'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:[]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': undefined, '': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[{b: 1}]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': {b:1}, '': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[{b: []}]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': {b:[]}, '': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromReverseUnevenNestedEmptyArray) {
        BSONObj keyPattern = fromjson("{'a.b': 1, 'a': 1}");
        BSONObj genKeysFrom = fromjson("{a:[]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null, '': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, SparseReverseUnevenNestedEmptyArray) {
        BSONObj keyPattern = fromjson("{'a.b': 1, 'a': 1}");
        BSONObj genKeysFrom = fromjson("{a:[]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null, '': undefined}"));
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromSparseEmptyArray) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:1}");
        BSONObjSet expectedKeys;
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));

        genKeysFrom = fromjson("{a:[]}");
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));

        genKeysFrom = fromjson("{a:[{c:1}]}");
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromSparseEmptyArraySecond) {
        BSONObj keyPattern = fromjson("{z: 1, 'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:1}");
        BSONObjSet expectedKeys;
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));

        genKeysFrom = fromjson("{a:[]}");
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));

        genKeysFrom = fromjson("{a:[{c:1}]}");
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));
    }

    TEST(BtreeKeyGeneratorTest, SparseNonObjectMissingNestedField) {
        BSONObj keyPattern = fromjson("{'a.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:[]}");
        BSONObjSet expectedKeys;
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));

        genKeysFrom = fromjson("{a:[1]}");
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));

        genKeysFrom = fromjson("{a:[1,{b:1}]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1}"));
        // true means sparse
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys, true));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromIndexedArrayIndex) {
        BSONObj keyPattern = fromjson("{'a.0': 1}");
        BSONObj genKeysFrom = fromjson("{a:[1]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[1]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': [1]}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:{'0':1}}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[{'0':1}]}");
        expectedKeys.clear();
        ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys), UserException);

        genKeysFrom = fromjson("{a:[1,{'0':2}]}");
        ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys), UserException);
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromDoubleIndexedArrayIndex) {
        BSONObj keyPattern = fromjson("{'a.0.0': 1}");
        BSONObj genKeysFrom = fromjson("{a:[[1]]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': null}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[[]]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromObjectWithinArray) {
        BSONObj keyPattern = fromjson("{'a.0.b': 1}");
        BSONObj genKeysFrom = fromjson("{a:[{b:1}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[{b:[1]}]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[{b:[[1]]}]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': [1]}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[{b:1}]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[{b:[1]}]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[{b:[[1]]}]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': [1]}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));

        genKeysFrom = fromjson("{a:[[{b:[]}]]}");
        expectedKeys.clear();
        expectedKeys.insert(fromjson("{'': undefined}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysFromArrayWithinObjectWithinArray) {
        BSONObj keyPattern = fromjson("{'a.0.b.0': 1}");
        BSONObj genKeysFrom = fromjson("{a:[{b:[1]}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': 1}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, ParallelArraysInNestedObjects) {
        BSONObj keyPattern = fromjson("{'a.a': 1, 'b.a': 1}");
        BSONObj genKeysFrom = fromjson("{a:{a:[1]}, b:{a:[1]}}");
        BSONObjSet expectedKeys;
        ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys), UserException);
    }

    TEST(BtreeKeyGeneratorTest, ParallelArraysUneven) {
        BSONObj keyPattern = fromjson("{'b.a': 1, 'a': 1}");
        BSONObj genKeysFrom = fromjson("{b:{a:[1]}, a:[1,2]}");
        BSONObjSet expectedKeys;
        ASSERT_THROWS(testKeygen(keyPattern, genKeysFrom, expectedKeys), UserException);
    }

    TEST(BtreeKeyGeneratorTest, MultipleArraysNotParallel) {
        BSONObj keyPattern = fromjson("{'a.b.c': 1}");
        BSONObj genKeysFrom = fromjson("{a: [1, 2, {b: {c: [3, 4]}}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null}"));
        expectedKeys.insert(fromjson("{'': 3}"));
        expectedKeys.insert(fromjson("{'': 4}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, MultipleArraysNotParallelCompound) {
        BSONObj keyPattern = fromjson("{'a.b.c': 1, 'a.b.d': 1}");
        BSONObj genKeysFrom = fromjson("{a: [1, 2, {b: {c: [3, 4], d: 5}}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'': null, '': null}"));
        expectedKeys.insert(fromjson("{'': 3, '': 5}"));
        expectedKeys.insert(fromjson("{'': 4, '': 5}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

    TEST(BtreeKeyGeneratorTest, GetKeysComplexNestedArrays) {
        BSONObj keyPattern = fromjson(
            "{'a.b.c.d': 1, 'a.g': 1, 'a.b.f': 1, 'a.b.c': 1, 'a.b.e': 1}");
        BSONObj genKeysFrom = fromjson(
            "{a: [1, {b: [2, {c: [3, {d: 1}], e: 4}, 5, {f: 6}], g: 7}]}");
        BSONObjSet expectedKeys;
        expectedKeys.insert(fromjson("{'':null, '':null, '':null, '':null, '':null}"));
        expectedKeys.insert(fromjson("{'':null, '':7, '':null, '':null, '':null}"));
        expectedKeys.insert(fromjson("{'':null, '':7, '':null, '':3, '':4}"));
        expectedKeys.insert(fromjson("{'':null, '':7, '':6, '':null, '':null}"));
        expectedKeys.insert(fromjson("{'':1, '':7, '':null, '':{d: 1}, '':4}"));
        ASSERT(testKeygen(keyPattern, genKeysFrom, expectedKeys));
    }

} // namespace
