// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
