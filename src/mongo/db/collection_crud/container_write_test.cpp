// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/collection_crud/container_write.h"

#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/stub_container.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <span>
#include <string>
#include <vector>

namespace mongo {
namespace {

using namespace std::string_view_literals;

std::span<const char> span(const std::string& s) {
    return std::span<const char>{s.data(), s.size()};
}

std::string_view view(std::span<const char> s) {
    return std::string_view{s.data(), s.size()};
}

std::vector<std::span<const char>> spans(const std::vector<std::string>& strings) {
    std::vector<std::span<const char>> out;
    out.reserve(strings.size());
    for (const auto& s : strings) {
        out.push_back(span(s));
    }
    return out;
}

constexpr auto kIdent = "internal-containerWriteTest"sv;

class ContainerWriteTest : public CatalogTestFixture {
public:
    void setUp() override {
        CatalogTestFixture::setUp();

        auto registry = std::make_unique<OpObserverRegistry>();
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
        getServiceContext()->resetOpObserver_forTest(std::move(registry));

        _integerContainer.setIdent(std::make_shared<Ident>(kIdent));
        _stringContainer.setIdent(std::make_shared<Ident>(kIdent));
    }

    IntegerKeyedContainer& integerContainer() {
        return _integerContainer;
    }

    StringKeyedContainer& stringContainer() {
        return _stringContainer;
    }

    RecoveryUnit& recoveryUnit() {
        return *shard_role_details::getRecoveryUnit(operationContext());
    }

    /**
     * Returns the container insert entries currently in the oplog, in the order they were written,
     * asserting that there are exactly 'numExpected' of them. The returned 'o' objects are parsed
     * through the container oplog entry IDL, so a malformed entry fails here.
     *
     * The BSONObjs backing the parsed views are kept alive in 'entries'.
     */
    std::vector<repl::ContainerInsertOplogEntryO> getNContainerInsertEntries(
        int numExpected, std::vector<BSONObj>& entries) {
        repl::OplogInterfaceLocal oplogInterface(operationContext());
        auto iter = oplogInterface.makeIterator();
        std::vector<BSONObj> reversed;
        while (true) {
            auto swEntry = iter->next();
            if (swEntry.getStatus() == ErrorCodes::CollectionIsEmpty) {
                break;
            }
            reversed.push_back(swEntry.getValue().first.getOwned());
        }
        // The oplog iterator returns entries newest-first.
        entries.assign(reversed.rbegin(), reversed.rend());

        std::vector<repl::ContainerInsertOplogEntryO> parsed;
        for (const auto& entry : entries) {
            const auto oplogEntry = unittest::assertGet(repl::OplogEntry::parse(entry));
            ASSERT_EQ(oplogEntry.getOpType(), repl::OpTypeEnum::kContainerInsert) << entry;
            ASSERT_EQ(oplogEntry.getEntry().getContainer(), kIdent) << entry;
            parsed.push_back(repl::ContainerInsertOplogEntryO::parse(
                oplogEntry.getObject(), IDLParserContext("ContainerInsertOplogEntryO")));
        }
        BSONArrayBuilder bab;
        for (const auto& entry : entries) {
            bab.append(entry);
        }
        ASSERT_EQ(parsed.size(), static_cast<size_t>(numExpected)) << bab.arr();
        return parsed;
    }

private:
    unittest::ServerParameterGuard _containerWrites{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard _batchedContainerWrites{"featureFlagBatchedContainerWrites",
                                                           true};

    StubIntegerKeyedContainer _integerContainer;
    StubStringKeyedContainer _stringContainer;
};

// An integer-keyed batch whose keys form one contiguous run collapses into a single oplog entry
// carrying the first key and the array of values.
TEST_F(ContainerWriteTest, IntegerKeyedBatchWithContiguousKeysEmitsSingleOplogEntry) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<int64_t> keys{100, 101, 102};
    const std::vector<std::string> values{"a", "bb", "ccc"};
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), integerContainer(), keys, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(1, raw);

    ASSERT_TRUE(entries[0].getKey().isIntKey());
    ASSERT_EQ(entries[0].getKey().getIntKey(), 100);

    ASSERT_TRUE(entries[0].getValue().has_value());
    ASSERT_TRUE(entries[0].getValue()->isArrayVal());
    const auto& vals = entries[0].getValue()->getArrayVal();
    ASSERT_EQ(vals.size(), values.size());
    for (size_t i = 0; i < vals.size(); ++i) {
        EXPECT_EQ(view(vals[i]), values[i]);
    }
}

// A string-keyed batch whose entries all share one value collapses into a single oplog entry
// carrying the array of keys and the shared value.
TEST_F(ContainerWriteTest, StringKeyedBatchWithIdenticalValuesEmitsSingleOplogEntry) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<std::string> keys{"k0", "k1", "k2"};
    const std::string sharedValue = "shared";
    const std::vector<std::string> values(keys.size(), sharedValue);
    const auto keySpans = spans(keys);
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), stringContainer(), keySpans, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(1, raw);

    ASSERT_TRUE(entries[0].getKey().isArrayKey());
    const auto& parsedKeys = entries[0].getKey().getArrayKey();
    ASSERT_EQ(parsedKeys.size(), keys.size());
    for (size_t i = 0; i < parsedKeys.size(); ++i) {
        EXPECT_EQ(view(parsedKeys[i]), keys[i]);
    }

    ASSERT_TRUE(entries[0].getValue().has_value());
    ASSERT_TRUE(entries[0].getValue()->isBytesVal());
    EXPECT_EQ(view(entries[0].getValue()->data()), sharedValue);
}

// Empty values are omitted from the entry entirely, which is the shape index-side containers write.
TEST_F(ContainerWriteTest, StringKeyedBatchWithEmptyValuesOmitsValueField) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<std::string> keys{"k0", "k1", "k2"};
    const std::vector<std::string> values(keys.size(), "");
    const auto keySpans = spans(keys);
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), stringContainer(), keySpans, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(1, raw);

    ASSERT_TRUE(entries[0].getKey().isArrayKey());
    ASSERT_EQ(entries[0].getKey().getArrayKey().size(), keys.size());
    ASSERT_FALSE(entries[0].getValue().has_value());
}

// A gap in the key sequence cannot be expressed by the (first key, value array) format, so the
// batch must be split at the gap.
TEST_F(ContainerWriteTest, IntegerKeyedBatchWithKeyGapEmitsOneOplogEntryPerRun) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<int64_t> keys{100, 101, 200, 201, 202};
    const std::vector<std::string> values{"a", "b", "c", "d", "e"};
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), integerContainer(), keys, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(2, raw);

    ASSERT_EQ(entries[0].getKey().getIntKey(), 100);
    const auto& firstVals = entries[0].getValue()->getArrayVal();
    ASSERT_EQ(firstVals.size(), 2u);
    EXPECT_EQ(view(firstVals[0]), "a");
    EXPECT_EQ(view(firstVals[1]), "b");

    ASSERT_EQ(entries[1].getKey().getIntKey(), 200);
    const auto& secondVals = entries[1].getValue()->getArrayVal();
    ASSERT_EQ(secondVals.size(), 3u);
    EXPECT_EQ(view(secondVals[0]), "c");
    EXPECT_EQ(view(secondVals[1]), "d");
    EXPECT_EQ(view(secondVals[2]), "e");
}

// A gap immediately before the last key still has to split; the trailing key is its own run.
TEST_F(ContainerWriteTest, IntegerKeyedBatchWithKeyGapBeforeLastKeyEmitsOneOplogEntryPerRun) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<int64_t> keys{100, 101, 200};
    const std::vector<std::string> values{"a", "b", "c"};
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), integerContainer(), keys, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(2, raw);

    ASSERT_EQ(entries[0].getKey().getIntKey(), 100);
    const auto& firstVals = entries[0].getValue()->getArrayVal();
    ASSERT_EQ(firstVals.size(), 2u);
    EXPECT_EQ(view(firstVals[0]), "a");
    EXPECT_EQ(view(firstVals[1]), "b");

    ASSERT_EQ(entries[1].getKey().getIntKey(), 200);
    ASSERT_TRUE(entries[1].getValue()->isArrayVal());
    const auto& secondVals = entries[1].getValue()->getArrayVal();
    ASSERT_EQ(secondVals.size(), 1u);
    EXPECT_EQ(view(secondVals[0]), "c");
}

// Non-contiguous keys throughout degenerate to one oplog entry per key.
TEST_F(ContainerWriteTest, IntegerKeyedBatchWithNoContiguousKeysEmitsOneOplogEntryPerKey) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<int64_t> keys{10, 20, 30};
    const std::vector<std::string> values{"a", "b", "c"};
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), integerContainer(), keys, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(3, raw);

    for (size_t i = 0; i < entries.size(); ++i) {
        EXPECT_EQ(entries[i].getKey().getIntKey(), keys[i]);
        const auto& vals = entries[i].getValue()->getArrayVal();
        ASSERT_EQ(vals.size(), 1u);
        EXPECT_EQ(view(vals[0]), values[i]);
    }
}

// The (key array, single value) format can only describe keys that share a value, so a change of
// value must split the batch.
TEST_F(ContainerWriteTest, StringKeyedBatchWithDifferingValuesEmitsOneOplogEntryPerRun) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<std::string> keys{"k0", "k1", "k2", "k3"};
    const std::vector<std::string> values{"v1", "v1", "v2", "v2"};
    const auto keySpans = spans(keys);
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), stringContainer(), keySpans, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(2, raw);

    const auto& firstKeys = entries[0].getKey().getArrayKey();
    ASSERT_EQ(firstKeys.size(), 2u);
    EXPECT_EQ(view(firstKeys[0]), "k0");
    EXPECT_EQ(view(firstKeys[1]), "k1");
    EXPECT_EQ(view(entries[0].getValue()->data()), "v1");

    const auto& secondKeys = entries[1].getKey().getArrayKey();
    ASSERT_EQ(secondKeys.size(), 2u);
    EXPECT_EQ(view(secondKeys[0]), "k2");
    EXPECT_EQ(view(secondKeys[1]), "k3");
    EXPECT_EQ(view(entries[1].getValue()->data()), "v2");
}

// A value change on the last element still has to split; the trailing key is its own run.
TEST_F(ContainerWriteTest, StringKeyedBatchWithDifferingLastValueEmitsOneOplogEntryPerRun) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<std::string> keys{"k0", "k1", "k2"};
    const std::vector<std::string> values{"v1", "v1", "v2"};
    const auto keySpans = spans(keys);
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), stringContainer(), keySpans, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(2, raw);

    const auto& firstKeys = entries[0].getKey().getArrayKey();
    ASSERT_EQ(firstKeys.size(), 2u);
    EXPECT_EQ(view(firstKeys[0]), "k0");
    EXPECT_EQ(view(firstKeys[1]), "k1");
    EXPECT_EQ(view(entries[0].getValue()->data()), "v1");

    const auto& secondKeys = entries[1].getKey().getArrayKey();
    ASSERT_EQ(secondKeys.size(), 1u);
    EXPECT_EQ(view(secondKeys[0]), "k2");
    EXPECT_EQ(view(entries[1].getValue()->data()), "v2");
}

// A single-element batch takes the same array-shaped path and produces exactly one entry.
TEST_F(ContainerWriteTest, SingleElementBatchesEmitOneOplogEntry) {
    Lock::GlobalLock lock{operationContext(), MODE_IX};

    const std::vector<int64_t> intKeys{7};
    const std::vector<std::string> values{"only"};
    const auto valueSpans = spans(values);

    ASSERT_OK(container_write::insert(
        operationContext(), recoveryUnit(), integerContainer(), intKeys, valueSpans));

    std::vector<BSONObj> raw;
    const auto entries = getNContainerInsertEntries(1, raw);
    ASSERT_EQ(entries[0].getKey().getIntKey(), 7);
    const auto& vals = entries[0].getValue()->getArrayVal();
    ASSERT_EQ(vals.size(), 1u);
    EXPECT_EQ(view(vals[0]), "only");
}

}  // namespace
}  // namespace mongo
