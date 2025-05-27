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

#include "mongo/db/exec/sbe/sbe_pattern_value_cmp.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/docval_to_sbeval.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mongo::sbe::value {
namespace {

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
        auto [specTag, specVal] = stage_builder::makeValue(sortPattern);
        value::ValueGuard guard{specTag, specVal};
        std::sort(
            sortedObjs.begin(), sortedObjs.end(), SbePatternValueCmp(specTag, specVal, collator));
    }

    std::pair<TypeTags, Value> getOrigObj(size_t i) {
        return objs[i];
    }

    std::pair<TypeTags, Value> getSortedObj(size_t i) {
        return sortedObjs[i];
    }

    static void assertValueEq(std::pair<TypeTags, Value> lhs, std::pair<TypeTags, Value> rhs) {
        auto [cmpTag, cmpVal] = value::compareValue(lhs.first, lhs.second, rhs.first, rhs.second);
        ASSERT(cmpTag == value::TypeTags::NumberInt32 && value::bitcastTo<int32_t>(cmpVal) == 0);
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

    assertValueEq(getOrigObj(0), getSortedObj(0));
    assertValueEq(getOrigObj(1), getSortedObj(2));
    assertValueEq(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, MixedOrder) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, a:1}"));
    addObj(fromjson("{a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortArray(fromjson("{b:1,a:-1}"), collator);

    assertValueEq(getOrigObj(0), getSortedObj(0));
    assertValueEq(getOrigObj(1), getSortedObj(1));
    assertValueEq(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, ExtraFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{b:1, c:2, a:1}"));
    addObj(fromjson("{c:1, a:3, b:2}"));
    addObj(fromjson("{b:3, a:2}"));

    sortArray(fromjson("{a:1,b:1}"), collator);

    assertValueEq(getOrigObj(0), getSortedObj(0));
    assertValueEq(getOrigObj(1), getSortedObj(2));
    assertValueEq(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, MissingFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:2, b:2}"));
    addObj(fromjson("{a:1}"));
    addObj(fromjson("{a:3, b:3, c:3}"));

    sortArray(fromjson("{b:1,c:1}"), collator);

    assertValueEq(getOrigObj(0), getSortedObj(1));
    assertValueEq(getOrigObj(1), getSortedObj(0));
    assertValueEq(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, NestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortArray(fromjson("{'a.b':1}"), collator);

    assertValueEq(getOrigObj(0), getSortedObj(1));
    assertValueEq(getOrigObj(1), getSortedObj(0));
    assertValueEq(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, SimpleNestedFields) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b: -1}}"));
    addObj(fromjson("{a:{b: -100}}"));
    addObj(fromjson("{a:{b: 34}}"));

    sortArray(fromjson("{'a.b':1}"), collator);

    assertValueEq(getOrigObj(0), getSortedObj(1));
    assertValueEq(getOrigObj(1), getSortedObj(0));
    assertValueEq(getOrigObj(2), getSortedObj(2));
}

TEST_F(ObjectArray, NestedInnerObjectDescending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortArray(fromjson("{'a.b.d':-1}"), collator);

    assertValueEq(getOrigObj(0), getSortedObj(2));
    assertValueEq(getOrigObj(1), getSortedObj(0));
    assertValueEq(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, NestedInnerObjectAscending) {
    const CollatorInterface* collator = nullptr;
    addObj(fromjson("{a:{b:{c:2, d:0}}}"));
    addObj(fromjson("{a:{b:{c:1, d:2}}}"));
    addObj(fromjson("{a:{b:{c:3, d:1}}}"));

    sortArray(fromjson("{'a.b.d':1}"), collator);

    assertValueEq(getOrigObj(0), getSortedObj(0));
    assertValueEq(getOrigObj(2), getSortedObj(1));
    assertValueEq(getOrigObj(1), getSortedObj(2));
}

TEST_F(ObjectArray, SortRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addObj(fromjson("{a: 'abg'}"));
    addObj(fromjson("{a: 'aca'}"));
    addObj(fromjson("{a: 'adc'}"));

    sortArray(fromjson("{a: 1}"), &collator);

    assertValueEq(getOrigObj(0), getSortedObj(2));
    assertValueEq(getOrigObj(1), getSortedObj(0));
    assertValueEq(getOrigObj(2), getSortedObj(1));
}

TEST_F(ObjectArray, SortSingleRespectsCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addValue(mongo::Value("abg"_sd));
    addValue(mongo::Value("aca"_sd));
    addValue(mongo::Value("adc"_sd));

    sortArray(fromjson("{'': 1}"), &collator);

    assertValueEq(getOrigObj(0), getSortedObj(2));
    assertValueEq(getOrigObj(1), getSortedObj(0));
    assertValueEq(getOrigObj(2), getSortedObj(1));
}

}  // namespace
}  // namespace mongo::sbe::value
