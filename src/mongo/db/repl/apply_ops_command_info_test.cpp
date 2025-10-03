/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
