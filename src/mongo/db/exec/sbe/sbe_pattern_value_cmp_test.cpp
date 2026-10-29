// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/sbe_pattern_value_cmp.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace mongo::sbe::value {
namespace {
using namespace std::literals::string_view_literals;

class ObjectArray : public mongo::unittest::Test {
public:
    ObjectArray() {}
    ~ObjectArray() override {
        for (auto& pair : objs) {
            releaseValue(pair.first, pair.second);
        }
    }

    void addObj(const BSONObj& obj) {
        objs.push_back(stage_builder::makeValue(obj));
    }

    void addValue(const mongo::Value& val) {
        objs.push_back(sbe::value::makeValue(val));
    }

    void sortArray(const BSONObj& sortPattern, const CollatorInterface* collator) {
        sortedObjs = objs;
        value::TagValueOwned spec =
            value::TagValueOwned::fromRaw(stage_builder::makeValue(sortPattern));
        std::sort(sortedObjs.begin(),
                  sortedObjs.end(),
                  SbePatternValueCmp(spec.tag(), spec.value(), collator));
    }

    TagValueView getOrigObj(size_t i) {
        return rawToView(objs[i]);
    }

    TagValueView getSortedObj(size_t i) {
        return rawToView(sortedObjs[i]);
    }

private:
    std::vector<std::pair<TypeTags, Value>> objs;
    std::vector<std::pair<TypeTags, Value>> sortedObjs;
};

TEST_F(ObjectArray, NormalOrder) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, a:1}"));
    addObj(fromjson("{a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortArray(fromjson("{'a':1,'b':1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(2));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(1));
    ASSERT_SBE_VALUE_LT(getSortedObj(0), getSortedObj(1));
    ASSERT_SBE_VALUE_GT(getSortedObj(1), getSortedObj(0));
}

TEST_F(ObjectArray, MixedOrder) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, a:1}"));
    addObj(fromjson("{a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortArray(fromjson("{b:1,a:-1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(1));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, ExtraFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, c:2, a:1}"));
    addObj(fromjson("{c:1, a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortArray(fromjson("{a:1,b:1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(2));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, MissingFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:2, b:2}"));
    addObj(fromjson("{a:1}"));
    addObj(fromjson("{a:3, b:3, c:3}"));

    sortArray(fromjson("{b:1,c:1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(1));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, NestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortArray(fromjson("{'a.b':1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(1));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, SimpleNestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b: -1}}"));
    addObj(fromjson("{a:{b: -100}}"));
    addObj(fromjson("{a:{b: 34}}"));

    sortArray(fromjson("{'a.b':1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(1));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, NestedInnerObjectDescending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortArray(fromjson("{'a.b.d':-1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(2));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, NestedInnerObjectAscending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortArray(fromjson("{'a.b.d':1}"), collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(1));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(2));
}

TEST_F(ObjectArray, SortRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addObj(fromjson("{a: 'abg'}"));
    addObj(fromjson("{a: 'aca'}"));
    addObj(fromjson("{a: 'adc'}"));

    sortArray(fromjson("{a: 1}"), &collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(2));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, SortSingleRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addValue(mongo::Value("abg"sv));
    addValue(mongo::Value("aca"sv));
    addValue(mongo::Value("adc"sv));

    sortArray(fromjson("{'': 1}"), &collator);

    ASSERT_SBE_VALUE_EQ(getOrigObj(0), getSortedObj(2));
    ASSERT_SBE_VALUE_EQ(getOrigObj(1), getSortedObj(0));
    ASSERT_SBE_VALUE_EQ(getOrigObj(2), getSortedObj(1));
}

}  // namespace
}  // namespace mongo::sbe::value
