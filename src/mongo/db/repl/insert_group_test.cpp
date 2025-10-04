/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
        &applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionOK);
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
        &applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionOK);
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
        &applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionError);
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
        &applyOps, nullptr, OplogApplication::Mode::kSecondary, true, testApplierFunctionOK);
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
