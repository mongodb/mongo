// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/apply_ops_command_info.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo::repl::apply_ops_command_info_details {
TEST(ApplyOpsCommandInfoTest, TestParseAreOpsCrudOnlySingleOps) {
    const BSONObj insertOp = BSON("applyOps" << BSON_ARRAY(BSON("op" << "i")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(insertOp));

    const BSONObj containerInsertOp = BSON("applyOps" << BSON_ARRAY(BSON("op" << "ci")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(containerInsertOp));

    const BSONObj deleteOp = BSON("applyOps" << BSON_ARRAY(BSON("op" << "d")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(deleteOp));

    const BSONObj containerDeleteOp = BSON("applyOps" << BSON_ARRAY(BSON("op" << "cd")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(containerDeleteOp));

    const BSONObj updateOp = BSON("applyOps" << BSON_ARRAY(BSON("op" << "u")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(updateOp));

    const BSONObj containerUpdateOp = BSON("applyOps" << BSON_ARRAY(BSON("op" << "cu")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(containerUpdateOp));

    const BSONObj noop = BSON("applyOps" << BSON_ARRAY(BSON("op" << "n")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(noop));

    const BSONObj commandOp = BSON("applyOps" << BSON_ARRAY(BSON("op" << "c")));
    ASSERT_FALSE(_parseAreOpsCrudOnly(commandOp));
}

TEST(ApplyOpsCommandInfoTest, TestParseAreOpsCrudOnlyMultipleOps) {
    const BSONObj allCrudOps =
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i") << BSON("op" << "n") << BSON("op" << "d")));
    ASSERT_TRUE(_parseAreOpsCrudOnly(allCrudOps));

    const BSONObj notAllCrudOps =
        BSON("applyOps" << BSON_ARRAY(BSON("op" << "i") << BSON("op" << "n") << BSON("op" << "d")
                                                        << BSON("op" << "c")));
    ASSERT_FALSE(_parseAreOpsCrudOnly(notAllCrudOps));
}

}  // namespace mongo::repl::apply_ops_command_info_details

namespace mongo::repl {
namespace {

TEST(ApplyOpsChainHelpersTest, RemainingApplyOpsChainOpsSaturatesAtZero) {
    // The common case: more still to collect than already collected.
    ASSERT_EQ(3U, remainingApplyOpsChainOps(5, 2));
    ASSERT_EQ(1U, remainingApplyOpsChainOps(5, 4));

    // Exactly collected, and the underflow guard: collected must never wrap to a huge size_t,
    // which would send a chain walk looking for entries that do not exist.
    ASSERT_EQ(0U, remainingApplyOpsChainOps(5, 5));
    ASSERT_EQ(0U, remainingApplyOpsChainOps(2, 5));
    ASSERT_EQ(0U, remainingApplyOpsChainOps(0, 0));
}

TEST(ApplyOpsChainHelpersTest, NumOperationsInApplyOpsCountsEntries) {
    ASSERT_EQ(1U, numOperationsInApplyOps(Value(BSON_ARRAY(BSON("op" << "i")))));
    ASSERT_EQ(3U,
              numOperationsInApplyOps(
                  Value(BSON_ARRAY(BSON("op" << "i") << BSON("op" << "u") << BSON("op" << "d")))));

    // A missing applyOps array (e.g. a commitTransaction entry) counts as zero operations.
    ASSERT_EQ(0U, numOperationsInApplyOps(Value()));
}

}  // namespace
}  // namespace mongo::repl

