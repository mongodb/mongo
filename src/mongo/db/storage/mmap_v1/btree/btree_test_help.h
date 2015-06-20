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

#pragma once

#include <string>

#include "mongo/db/json.h"
#include "mongo/db/storage/mmap_v1/btree/btree_logic.h"
#include "mongo/db/storage/mmap_v1/heap_record_store_btree.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_test_help.h"

namespace mongo {

/**
 * Generates a string of the specified length containing repeated concatenation of the
 * hexadecimal representation of the input value.
 */
std::string bigNumString(long long n, int len);

/**
 * Generates key on a field 'a', with the specified number of repetitions of the character.
 */
BSONObj simpleKey(char c, int n = 1);

/**
 * Simple head manager, which performs no validity checking or persistence.
 */
class TestHeadManager : public HeadManager {
public:
    virtual const RecordId getHead(OperationContext* txn) const {
        return _head;
    }

    virtual void setHead(OperationContext* txn, const RecordId newHead) {
        _head = newHead;
    }

private:
    RecordId _head;
};


/**
 * This structure encapsulates a Btree and all the infrastructure needed by it (head manager,
 * record store and a valid disk location to use by the tests).
 */
template <class OnDiskFormat>
struct BtreeLogicTestHelper {
    BtreeLogicTestHelper(const BSONObj& order);

    // Everything needed for a fully-functional Btree logic
    TestHeadManager headManager;
    HeapRecordStoreBtree recordStore;
    SavedCursorRegistry cursorRegistry;
    BtreeLogic<OnDiskFormat> btree;
    DiskLoc dummyDiskLoc;
};


/**
 * Tool to construct custom tree shapes for tests.
 */
template <class OnDiskFormat>
class ArtificialTreeBuilder {
public:
    typedef typename BtreeLogic<OnDiskFormat>::BucketType BucketType;
    typedef typename BtreeLogic<OnDiskFormat>::KeyDataOwnedType KeyDataOwnedType;
    typedef typename BtreeLogic<OnDiskFormat>::KeyHeaderType KeyHeaderType;

    typedef typename OnDiskFormat::FixedWidthKeyType FixedWidthKeyType;

    /**
     * The tree builder wraps around the passed-in helper and will invoke methods on it. It
     * does not do any cleanup, so constructing multiple trees over the same helper will
     * cause leaked records.
     */
    ArtificialTreeBuilder(OperationContext* txn, BtreeLogicTestHelper<OnDiskFormat>* helper)
        : _txn(txn), _helper(helper) {}

    /**
     * Causes the specified tree shape to be built on the associated helper and the tree's
     * root installed as the head. Uses a custom JSON-based language with the following
     * syntax:
     *
     * Btree := BTreeBucket
     * BtreeBucket := { Child_1_Key: <BtreeBucket | null>,
     *                  Child_2_Key: <BtreeBucket | null>,
     *                  ...,
     *                  _: <BtreeBucket | null> }
     *
     * The _ key name specifies the content of the nextChild pointer. The value null means
     * use a fixed disk loc.
     */
    void makeTree(const std::string& spec);

    /**
     * Validates that the structure of the Btree in the helper matches the specification.
     */
    void checkStructure(const std::string& spec) const;

    /**
     * Adds the following key to the bucket and fixes up the child pointers.
     */
    void push(const DiskLoc bucketLoc, const BSONObj& key, const DiskLoc child);

    /**
     * @return The number of keys inserted.
     */
    int fillBucketToExactSize(const DiskLoc bucketLoc, int targetSize, char startKey);

private:
    DiskLoc makeTree(const BSONObj& spec);

    void checkStructure(const BSONObj& spec, const DiskLoc node) const;

    bool isPresent(const BSONObj& key, int direction) const;

    static std::string expectedKey(const char* spec);

    OperationContext* _txn;
    BtreeLogicTestHelper<OnDiskFormat>* _helper;
};

}  // namespace mongo
