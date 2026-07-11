// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/insert_group.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <vector>

#include <fmt/format.h>

namespace mongo {
namespace repl {
namespace {

const OpTime entryOpTime{Timestamp(3, 4), 5};
const std::vector<NamespaceString> nssV{
    NamespaceString::createNamespaceString_forTest("foo", "bar1"),
    NamespaceString::createNamespaceString_forTest("foo", "bar2")};
const int docId = 17;

Status testApplierFunctionOK(OperationContext* ops,
                             const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
                             OplogApplication::Mode mode,
                             const bool isDataConsistent) {
    return Status::OK();
}

Status testApplierFunctionError(OperationContext* ops,
                                const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
                                OplogApplication::Mode mode,
                                const bool isDataConsistent) {
    return Status(ErrorCodes::BadValue, "Error Injection");
}

void buildOps(const int numOps,
              const int docId,
              const NamespaceString nss,
              std::vector<mongo::repl::OplogEntry>& ops) {
    for (int i = 0; i < numOps; i++) {
        const BSONObj doc = BSON("_id" << docId + i);
        ops.push_back(makeInsertDocumentOplogEntry({Timestamp(Seconds(2), i), 1LL}, nss, doc));
    }
}

void buildApplierOperationsFromOps(std::vector<ApplierOperation>& applyOps,
                                   std::vector<mongo::repl::OplogEntry>& ops) {
    for (auto it = ops.begin(); it < ops.end(); it++) {
        const OplogEntry& op = *it;
        applyOps.push_back({&op});
    }
}

// Groupable with single namespace
TEST(InsertGroupTest, SingleGroupedInserts) {
    std::vector<ApplierOperation> applyOps;
    std::vector<mongo::repl::OplogEntry> ops;
    constexpr int numOps = 16;

    buildOps(numOps, docId, nssV[0], ops);
    buildApplierOperationsFromOps(applyOps, ops);
    InsertGroup insertGroup(
        applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionOK);
    int numGroups = 0;

    for (auto it = applyOps.cbegin(); it != applyOps.cend(); ++it, ++numGroups) {
        auto groupResult = insertGroup.groupAndApplyInserts(it);
        if (groupResult.isOK()) {
            // If we are successful in grouping and applying inserts, the iterator points to the
            // last entry of the group
            it = groupResult.getValue();
            continue;
        }
    }
    ASSERT(numGroups == 1);
}

// Groupable with multiple namespaces
TEST(InsertGroupTest, MultipleGroupedInserts) {
    std::vector<ApplierOperation> applyOps;
    std::vector<mongo::repl::OplogEntry> ops;
    constexpr int numOps = 16;

    for (const auto& it : nssV) {
        buildOps(numOps, docId, it, ops);
    }

    buildApplierOperationsFromOps(applyOps, ops);

    InsertGroup insertGroup(
        applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionOK);
    unsigned long numGroups = 0;

    for (auto it = applyOps.cbegin(); it != applyOps.cend(); ++it, ++numGroups) {
        auto groupResult = insertGroup.groupAndApplyInserts(it);
        if (groupResult.isOK()) {
            // If we are successful in grouping and applying inserts, the iterator points to the
            // last entry of the group
            it = groupResult.getValue();
            continue;
        }
    }
    ASSERT(numGroups == nssV.size());
}

// Non-groupable with applyInsert error
TEST(InsertGroupTest, NoGroupingApplyError) {
    std::vector<ApplierOperation> applyOps;
    std::vector<mongo::repl::OplogEntry> ops;

    constexpr int numOps = 8;
    buildOps(numOps, docId, nssV[0], ops);
    buildApplierOperationsFromOps(applyOps, ops);

    InsertGroup insertGroup(
        applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionError);
    int numGroups = 0;

    for (auto it = applyOps.cbegin(); it != applyOps.cend(); ++it, ++numGroups) {
        auto groupResult = insertGroup.groupAndApplyInserts(it);
        if (groupResult.isOK()) {
            // If we are successful in grouping and applying inserts, the iterator points to the
            // last entry of the group
            it = groupResult.getValue();
            continue;
        }
    }
    ASSERT(numGroups == numOps);
}

// Number of ops resulting in multiple groups
TEST(InsertGroupTest, NoGroupingCount) {
    std::vector<ApplierOperation> applyOps;
    std::vector<mongo::repl::OplogEntry> ops;
    constexpr int numOps = 67;

    buildOps(numOps, docId, nssV[0], ops);
    buildApplierOperationsFromOps(applyOps, ops);

    InsertGroup insertGroup(
        applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionOK);
    int numGroups = 0;

    for (auto it = applyOps.cbegin(); it != applyOps.cend(); ++it, ++numGroups) {
        auto groupResult = insertGroup.groupAndApplyInserts(it);
        if (groupResult.isOK()) {
            // If we are successful in grouping and applying inserts, the iterator points to the
            // last entry of the group
            it = groupResult.getValue();
            continue;
        }
    }
    ASSERT(numGroups == 2);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
