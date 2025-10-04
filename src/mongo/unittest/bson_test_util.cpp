/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/unittest/assert.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <ostream>

namespace mongo {
namespace unittest {

#define GENERATE_BSON_CMP_FUNC(BSONTYPE, NAME, COMPARATOR, OPERATOR)                 \
    void assertComparison_##BSONTYPE##NAME(const std::string& theFile,               \
                                           unsigned theLine,                         \
                                           StringData aExpression,                   \
                                           StringData bExpression,                   \
                                           const BSONTYPE& aValue,                   \
                                           const BSONTYPE& bValue) {                 \
        if (!COMPARATOR.evaluate(aValue OPERATOR bValue)) {                          \
            std::ostringstream os;                                                   \
            os << "Expected [ " << aExpression << " " #OPERATOR " " << bExpression   \
               << " ] but found [ " << aValue << " " #OPERATOR " " << bValue << "]"; \
            TestAssertionFailure(theFile, theLine, os.str()).stream();               \
        }                                                                            \
    }

GENERATE_BSON_CMP_FUNC(BSONObj, EQ, SimpleBSONObjComparator::kInstance, ==);
GENERATE_BSON_CMP_FUNC(BSONObj, LT, SimpleBSONObjComparator::kInstance, <);
GENERATE_BSON_CMP_FUNC(BSONObj, LTE, SimpleBSONObjComparator::kInstance, <=);
GENERATE_BSON_CMP_FUNC(BSONObj, GT, SimpleBSONObjComparator::kInstance, >);
GENERATE_BSON_CMP_FUNC(BSONObj, GTE, SimpleBSONObjComparator::kInstance, >=);
GENERATE_BSON_CMP_FUNC(BSONObj, NE, SimpleBSONObjComparator::kInstance, !=);

GENERATE_BSON_CMP_FUNC(BSONObj, EQ_UNORDERED, UnorderedFieldsBSONObjComparator{}, ==);
GENERATE_BSON_CMP_FUNC(BSONObj, LT_UNORDERED, UnorderedFieldsBSONObjComparator{}, <);
GENERATE_BSON_CMP_FUNC(BSONObj, LTE_UNORDERED, UnorderedFieldsBSONObjComparator{}, <=);
GENERATE_BSON_CMP_FUNC(BSONObj, GT_UNORDERED, UnorderedFieldsBSONObjComparator{}, >);
GENERATE_BSON_CMP_FUNC(BSONObj, GTE_UNORDERED, UnorderedFieldsBSONObjComparator{}, >=);
GENERATE_BSON_CMP_FUNC(BSONObj, NE_UNORDERED, UnorderedFieldsBSONObjComparator{}, !=);

// This comparator checks for binary equality. Useful when logical equality (through woCompare()) is
// not strong enough.
class BSONObjBinaryComparator final : public BSONObj::ComparatorInterface {
public:
    static const BSONObjBinaryComparator kInstance;

    /**
     * The function only supports equals to operation.
     */
    int compare(const BSONObj& lhs, const BSONObj& rhs) const final {
        return !lhs.binaryEqual(rhs);
    }
    void hash_combine(size_t& seed, const BSONObj& toHash) const final {
        MONGO_UNREACHABLE;
    }
};
const BSONObjBinaryComparator BSONObjBinaryComparator::kInstance{};

GENERATE_BSON_CMP_FUNC(BSONObj, BINARY_EQ, BSONObjBinaryComparator::kInstance, ==);

GENERATE_BSON_CMP_FUNC(BSONElement, EQ, SimpleBSONElementComparator::kInstance, ==);
GENERATE_BSON_CMP_FUNC(BSONElement, LT, SimpleBSONElementComparator::kInstance, <);
GENERATE_BSON_CMP_FUNC(BSONElement, LTE, SimpleBSONElementComparator::kInstance, <=);
GENERATE_BSON_CMP_FUNC(BSONElement, GT, SimpleBSONElementComparator::kInstance, >);
GENERATE_BSON_CMP_FUNC(BSONElement, GTE, SimpleBSONElementComparator::kInstance, >=);
GENERATE_BSON_CMP_FUNC(BSONElement, NE, SimpleBSONElementComparator::kInstance, !=);

std::string formatJsonStr(const std::string& input) {
    BSONObj obj = fromjson(input);
    const static JsonStringFormat format = JsonStringFormat::ExtendedRelaxedV2_0_0;
    std::string str = obj.jsonString(format);

    // Raw JSON strings additionally have `fromjson(R())`, so subtract that from the auto-update max
    // line length.
    static constexpr size_t kRawJsonMaxLineLength =
        kAutoUpdateMaxLineLength - "fromjson(R())"_sd.size();
    if (str.size() > kRawJsonMaxLineLength) {
        str = obj.jsonString(format, 1);
    }
    return str::stream() << "R\"(" << str << ")\"";
}

}  // namespace unittest
}  // namespace mongo
