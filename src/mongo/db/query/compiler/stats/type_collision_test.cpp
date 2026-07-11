// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/stats/max_diff.h"
#include "mongo/db/query/compiler/stats/value_utils.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

#include <string>
#include <string_view>
#include <vector>

namespace mongo::stats {
TEST(TypeCollisionTest, ZeroedCollidingTypesHistogram) {
    // Simplest example: empty strings map to 0.
    const std::string_view collider("");
    ASSERT_EQ(stringToDouble(collider), 0.0);

    // Note: this is contrived, and possibly an illegal value for an object id.
    const sbe::value::ObjectIdType oid{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ASSERT_EQ(objectIdToDouble(&oid), 0.0);

    std::vector<SBEValue> data{
        // Numeric types.
        sbe::value::makeValue(Value((double)0.0)),
        sbe::value::makeValue(Value((int)0)),
        sbe::value::makeValue(Value((long long)0)),
        sbe::value::makeValue(Value(Decimal128())),
        // Object ID.
        sbe::value::makeValue(Value(OID({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}))),
        // Date types: note that we can ensure their double equivalent is 0 by default-constructing
        // them.
        sbe::value::makeValue(Value(Date_t())),
        sbe::value::makeValue(Value(Timestamp())),
        // String types.
        sbe::value::makeValue(Value(std::string(""))),
    };

    // We should always fail to build a histogram on 0 buckets.
    auto i = 0;
    ASSERT_THROWS(createCEHistogram(data, 0), DBException);

    // We should always fail to build a histogram if we have fewer buckets than type classes + 1.
    for (i = 1; i < 6; i++) {
        ASSERT_THROWS(createCEHistogram(data, i), DBException);
    }

    // With sufficient buckets, we should build a histogram with one bucket per type class.
    auto ceHist = createCEHistogram(data, i);
    auto expected = fromjson(
        "{ \
		trueCount: 0.0, \
		falseCount: 0.0, \
		nanCount: 0.0, \
		emptyArrayCount: 0.0, \
		typeCount: [ \
			{ typeName: \"NumberInt32\", count: 1.0 }, \
			{ typeName: \"NumberInt64\", count: 1.0 }, \
			{ typeName: \"NumberDouble\", count: 1.0 }, \
			{ typeName: \"Date\", count: 1.0 }, \
			{ typeName: \"Timestamp\", count: 1.0 }, \
			{ typeName: \"StringSmall\", count: 1.0 }, \
			{ typeName: \"NumberDecimal\", count: 1.0 }, \
			{ typeName: \"ObjectId\", count: 1.0 } \
		], \
		scalarHistogram: { \
			buckets: [ \
				{ \
					boundaryCount: 4.0, \
					rangeCount: 0.0, \
					rangeDistincts: 0.0, \
					cumulativeCount: 4.0, \
					cumulativeDistincts: 1.0 \
				}, \
				{ \
					boundaryCount: 1.0, \
					rangeCount: 0.0, \
					rangeDistincts: 0.0, \
					cumulativeCount: 5.0, \
					cumulativeDistincts: 2.0 \
				}, \
				{ \
					boundaryCount: 1.0, \
					rangeCount: 0.0, \
					rangeDistincts: 0.0, \
					cumulativeCount: 6.0, \
					cumulativeDistincts: 3.0 \
				}, \
				{ \
					boundaryCount: 1.0, \
					rangeCount: 0.0, \
					rangeDistincts: 0.0, \
					cumulativeCount: 7.0, \
					cumulativeDistincts: 4.0 \
				}, \
				{ \
					boundaryCount: 1.0, \
					rangeCount: 0.0, \
					rangeDistincts: 0.0, \
					cumulativeCount: 8.0, \
					cumulativeDistincts: 5.0 \
				} \
			], \
			bounds: [0.0, \"\", ObjectId('000000000000000000000000'), new Date(0), Timestamp(0, 0)]\
		} \
	}");
    ASSERT_BSONOBJ_EQ(expected, ceHist->serialize());
}
}  // namespace mongo::stats
