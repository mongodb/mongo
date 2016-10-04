// record_store_v1_simple_test.cpp

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

#include "mongo/db/storage/mmap_v1/record_store_v1_simple.h"

#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/record.h"
#include "mongo/db/storage/mmap_v1/record_store_v1_test_help.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

using std::string;

TEST(SimpleRecordStoreV1, quantizeAllocationSpaceSimple) {
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(33), 64);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(1000), 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(10001), 16 * 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(100000), 128 * 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(1000001), 1024 * 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(10000000), 10 * 1024 * 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(14 * 1024 * 1024 - 1),
                  14 * 1024 * 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(14 * 1024 * 1024), 14 * 1024 * 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(14 * 1024 * 1024 + 1),
                  16 * 1024 * 1024 + 512 * 1024);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(16 * 1024 * 1024 + 512 * 1024),
                  16 * 1024 * 1024 + 512 * 1024);
}

TEST(SimpleRecordStoreV1, quantizeAllocationMinMaxBound) {
    const int maxSize = RecordStoreV1Base::MaxAllowedAllocation;
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(1), 32);
    ASSERT_EQUALS(RecordStoreV1Base::quantizeAllocationSpace(maxSize), maxSize);
}

/**
 * Tests quantization of sizes around all valid bucket sizes.
 */
TEST(SimpleRecordStoreV1, quantizeAroundBucketSizes) {
    for (int bucket = 0; bucket < RecordStoreV1Base::Buckets - 2; bucket++) {
        const int size = RecordStoreV1Base::bucketSizes[bucket];
        const int nextSize = RecordStoreV1Base::bucketSizes[bucket + 1];

        // size - 1 is quantized to size.
        ASSERT_EQUALS(size, RecordStoreV1Base::quantizeAllocationSpace(size - 1));

        // size is quantized to size.
        ASSERT_EQUALS(size, RecordStoreV1Base::quantizeAllocationSpace(size));

        // size + 1 is quantized to nextSize (if it is a valid allocation)
        if (size + 1 <= RecordStoreV1Base::MaxAllowedAllocation) {
            ASSERT_EQUALS(nextSize, RecordStoreV1Base::quantizeAllocationSpace(size + 1));
        }
    }
}

BSONObj docForRecordSize(int size) {
    BSONObjBuilder b;
    b.append("_id", 5);
    b.append("x", string(size - MmapV1RecordHeader::HeaderSize - 22, 'x'));
    BSONObj x = b.obj();
    ASSERT_EQUALS(MmapV1RecordHeader::HeaderSize + x.objsize(), size);
    return x;
}

class BsonDocWriter final : public DocWriter {
public:
    BsonDocWriter(const BSONObj& obj, bool padding) : _obj(obj), _padding(padding) {}

    virtual void writeDocument(char* buf) const {
        memcpy(buf, _obj.objdata(), _obj.objsize());
    }
    virtual size_t documentSize() const {
        return _obj.objsize();
    }
    virtual bool addPadding() const {
        return _padding;
    }

private:
    BSONObj _obj;
    bool _padding;
};

/** alloc() quantizes the requested size using quantizeAllocationSpace() rules. */
TEST(SimpleRecordStoreV1, AllocQuantized) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);

    string myns = "test.AllocQuantized";
    SimpleRecordStoreV1 rs(&txn, myns, md, &em, false);

    BSONObj obj = docForRecordSize(300);
    StatusWith<RecordId> result = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), false);
    ASSERT(result.isOK());

    // The length of the allocated record is quantized.
    ASSERT_EQUALS(512, rs.dataFor(&txn, result.getValue()).size() + MmapV1RecordHeader::HeaderSize);
}

TEST(SimpleRecordStoreV1, AllocNonQuantized) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    md->setUserFlag(&txn, CollectionOptions::Flag_NoPadding);

    string myns = "test.AllocQuantized";
    SimpleRecordStoreV1 rs(&txn, myns, md, &em, false);

    BSONObj obj = docForRecordSize(300);
    StatusWith<RecordId> result = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), false);
    ASSERT(result.isOK());

    // The length of the allocated record is quantized.
    ASSERT_EQUALS(300, rs.dataFor(&txn, result.getValue()).size() + MmapV1RecordHeader::HeaderSize);
}

TEST(SimpleRecordStoreV1, AllocNonQuantizedStillAligned) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    md->setUserFlag(&txn, CollectionOptions::Flag_NoPadding);

    string myns = "test.AllocQuantized";
    SimpleRecordStoreV1 rs(&txn, myns, md, &em, false);

    BSONObj obj = docForRecordSize(298);
    StatusWith<RecordId> result = rs.insertRecord(&txn, obj.objdata(), obj.objsize(), false);
    ASSERT(result.isOK());

    // The length of the allocated record is quantized.
    ASSERT_EQUALS(300, rs.dataFor(&txn, result.getValue()).size() + MmapV1RecordHeader::HeaderSize);
}

/** alloc() quantizes the requested size if DocWriter::addPadding() returns true. */
TEST(SimpleRecordStoreV1, AllocQuantizedWithDocWriter) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);

    string myns = "test.AllocQuantized";
    SimpleRecordStoreV1 rs(&txn, myns, md, &em, false);

    BsonDocWriter docWriter(docForRecordSize(300), true);
    StatusWith<RecordId> result = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT(result.isOK());

    // The length of the allocated record is quantized.
    ASSERT_EQUALS(512, rs.dataFor(&txn, result.getValue()).size() + MmapV1RecordHeader::HeaderSize);
}

/**
 * alloc() does not quantize records if DocWriter::addPadding() returns false
 */
TEST(SimpleRecordStoreV1, AllocNonQuantizedDocWriter) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);

    string myns = "test.AllocIndexNamespaceNotQuantized";
    SimpleRecordStoreV1 rs(&txn, myns + "$x", md, &em, false);

    BsonDocWriter docWriter(docForRecordSize(300), false);
    StatusWith<RecordId> result = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT(result.isOK());

    // The length of the allocated record is not quantized.
    ASSERT_EQUALS(300, rs.dataFor(&txn, result.getValue()).size() + MmapV1RecordHeader::HeaderSize);
}

/** alloc() aligns record sizes up to 4 bytes even if DocWriter::addPadding returns false. */
TEST(SimpleRecordStoreV1, AllocAlignedDocWriter) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);

    string myns = "test.AllocIndexNamespaceNotQuantized";
    SimpleRecordStoreV1 rs(&txn, myns + "$x", md, &em, false);

    BsonDocWriter docWriter(docForRecordSize(298), false);
    StatusWith<RecordId> result = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT(result.isOK());

    ASSERT_EQUALS(300, rs.dataFor(&txn, result.getValue()).size() + MmapV1RecordHeader::HeaderSize);
}
/**
 * alloc() with quantized size doesn't split if enough room left over.
 */
TEST(SimpleRecordStoreV1, AllocUseQuantizedDeletedRecordWithoutSplit) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 512 + 31}, {}};
        initializeV1RS(&txn, NULL, drecs, NULL, &em, md);
    }

    BsonDocWriter docWriter(docForRecordSize(300), true);
    StatusWith<RecordId> actualLocation = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT_OK(actualLocation.getStatus());

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 512 + 31}, {}};
        LocAndSize drecs[] = {{}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
    }
}

/**
 * alloc() with quantized size splits if enough room left over.
 */
TEST(SimpleRecordStoreV1, AllocUseQuantizedDeletedRecordWithSplit) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 512 + 32}, {}};
        initializeV1RS(&txn, NULL, drecs, NULL, &em, md);
    }

    BsonDocWriter docWriter(docForRecordSize(300), true);
    StatusWith<RecordId> actualLocation = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT_OK(actualLocation.getStatus());

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 512}, {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1512), 32}, {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
    }
}

/**
 * alloc() with non quantized size doesn't split if enough room left over.
 */
TEST(SimpleRecordStoreV1, AllocUseNonQuantizedDeletedRecordWithoutSplit) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 331}, {}};
        initializeV1RS(&txn, NULL, drecs, NULL, &em, md);
    }

    BsonDocWriter docWriter(docForRecordSize(300), false);
    StatusWith<RecordId> actualLocation = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT_OK(actualLocation.getStatus());

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 331}, {}};
        LocAndSize drecs[] = {{}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
    }
}

/**
 * alloc() with non quantized size splits if enough room left over.
 */
TEST(SimpleRecordStoreV1, AllocUseNonQuantizedDeletedRecordWithSplit) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 332}, {}};
        initializeV1RS(&txn, NULL, drecs, NULL, &em, md);
    }

    BsonDocWriter docWriter(docForRecordSize(300), false);
    StatusWith<RecordId> actualLocation = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT_OK(actualLocation.getStatus());

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 300}, {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1300), 32}, {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
    }
}

/**
 * alloc() will use from the legacy grab bag if it can.
 */
TEST(SimpleRecordStoreV1, GrabBagIsUsed) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize drecs[] = {{}};
        LocAndSize grabBag[] = {
            {DiskLoc(0, 1000), 4 * 1024 * 1024}, {DiskLoc(1, 1000), 4 * 1024 * 1024}, {}};
        initializeV1RS(&txn, NULL, drecs, grabBag, &em, md);
    }

    BsonDocWriter docWriter(docForRecordSize(256), false);
    StatusWith<RecordId> actualLocation = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT_OK(actualLocation.getStatus());

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 256}, {}};
        LocAndSize drecs[] = {{DiskLoc(0, 1256), 4 * 1024 * 1024 - 256}, {}};
        LocAndSize grabBag[] = {{DiskLoc(1, 1000), 4 * 1024 * 1024}, {}};
        assertStateV1RS(&txn, recs, drecs, grabBag, &em, md);
    }
}

/**
 * alloc() will pull from the legacy grab bag even if it isn't needed.
 */
TEST(SimpleRecordStoreV1, GrabBagIsPoppedEvenIfUnneeded) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 1000}, {}};
        LocAndSize grabBag[] = {
            {DiskLoc(1, 1000), 4 * 1024 * 1024}, {DiskLoc(2, 1000), 4 * 1024 * 1024}, {}};
        initializeV1RS(&txn, NULL, drecs, grabBag, &em, md);
    }

    BsonDocWriter docWriter(docForRecordSize(1000), false);
    StatusWith<RecordId> actualLocation = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT_OK(actualLocation.getStatus());

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 1000}, {}};
        LocAndSize drecs[] = {{DiskLoc(1, 1000), 4 * 1024 * 1024}, {}};
        LocAndSize grabBag[] = {{DiskLoc(2, 1000), 4 * 1024 * 1024}, {}};
        assertStateV1RS(&txn, recs, drecs, grabBag, &em, md);
    }
}

/**
 * alloc() will pull from the legacy grab bag even if it can't be used
 */
TEST(SimpleRecordStoreV1, GrabBagIsPoppedEvenIfUnusable) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize drecs[] = {{DiskLoc(0, 1000), 8 * 1024 * 1024}, {}};
        LocAndSize grabBag[] = {
            {DiskLoc(1, 1000), 4 * 1024 * 1024}, {DiskLoc(2, 1000), 4 * 1024 * 1024}, {}};
        initializeV1RS(&txn, NULL, drecs, grabBag, &em, md);
    }

    BsonDocWriter docWriter(docForRecordSize(8 * 1024 * 1024), false);
    StatusWith<RecordId> actualLocation = rs.insertRecordWithDocWriter(&txn, &docWriter);
    ASSERT_OK(actualLocation.getStatus());

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 8 * 1024 * 1024}, {}};
        LocAndSize drecs[] = {{DiskLoc(1, 1000), 4 * 1024 * 1024}, {}};
        LocAndSize grabBag[] = {{DiskLoc(2, 1000), 4 * 1024 * 1024}, {}};
        assertStateV1RS(&txn, recs, drecs, grabBag, &em, md);
    }
}

// -----------------

TEST(SimpleRecordStoreV1, FullSimple1) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);


    ASSERT_EQUALS(0, md->numRecords());
    StatusWith<RecordId> result = rs.insertRecord(&txn, "abc", 4, 1000);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(1, md->numRecords());
    RecordData recordData = rs.dataFor(&txn, result.getValue());
    ASSERT_EQUALS(string("abc"), string(recordData.data()));
}

// -----------------

TEST(SimpleRecordStoreV1, Truncate) {
    OperationContextNoop txn;
    DummyExtentManager em;
    DummyRecordStoreV1MetaData* md = new DummyRecordStoreV1MetaData(false, 0);
    SimpleRecordStoreV1 rs(&txn, "test.foo", md, &em, false);

    {
        LocAndSize recs[] = {{DiskLoc(0, 1000), 100},
                             {DiskLoc(0, 1100), 100},
                             {DiskLoc(0, 1300), 100},
                             {DiskLoc(2, 1100), 100},
                             {}};
        LocAndSize drecs[] = {
            {DiskLoc(0, 1200), 100}, {DiskLoc(2, 1000), 100}, {DiskLoc(1, 1000), 1000}, {}};

        initializeV1RS(&txn, recs, drecs, NULL, &em, md);

        ASSERT_EQUALS(em.getExtent(DiskLoc(0, 0))->length, em.minSize());
    }

    rs.truncate(&txn);

    {
        LocAndSize recs[] = {{}};
        LocAndSize drecs[] = {
            // One extent filled with a single deleted record.
            {DiskLoc(0, Extent::HeaderSize()), em.minSize() - Extent::HeaderSize()},
            {}};
        assertStateV1RS(&txn, recs, drecs, NULL, &em, md);
    }
}
}
