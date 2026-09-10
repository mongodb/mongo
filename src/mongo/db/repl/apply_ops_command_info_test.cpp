// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/apply_ops_command_info.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
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
