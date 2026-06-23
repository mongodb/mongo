/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/commands/query_cmd/explain_cmd_helpers.h"

#include "mongo/unittest/unittest.h"

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(ResolveMaxTimeMSTest, ZeroOrUnsetMeansNoLimit) {
    using explain_cmd_helpers::resolveMaxTimeMS;
    ASSERT_EQ(*resolveMaxTimeMS(100, 0), 100);
    ASSERT_EQ(*resolveMaxTimeMS(100, boost::none), 100);
    ASSERT_EQ(*resolveMaxTimeMS(0, 5), 5);
    ASSERT_EQ(*resolveMaxTimeMS(boost::none, 5), 5);
    ASSERT_EQ(*resolveMaxTimeMS(0, 0), 0);
    ASSERT_FALSE(resolveMaxTimeMS(boost::none, boost::none).has_value());
}

TEST(ResolveMaxTimeMSTest, ExplicitZeroIsPreservedOverUnset) {
    using explain_cmd_helpers::resolveMaxTimeMS;
    // An explicit 0 ("no limit") must survive even when the other placement is unset, so the
    // explain command keeps an explicit maxTimeMS and does not inherit defaultMaxTimeMS.
    ASSERT_EQ(*resolveMaxTimeMS(boost::none, 0), 0);
    ASSERT_EQ(*resolveMaxTimeMS(0, boost::none), 0);
}

}  // namespace
}  // namespace mongo
