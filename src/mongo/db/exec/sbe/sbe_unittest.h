/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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
#pragma once

#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/golden_test.h"
#include "mongo/unittest/matcher.h"
#include "mongo/unittest/unittest.h"

#include "mongo/db/exec/sbe/values/value.h"
#include <sstream>

namespace mongo::sbe {

using namespace mongo::unittest;
using namespace mongo::unittest::match;

using TypedValue = std::pair<value::TypeTags, value::Value>;

extern unittest::GoldenTestConfig goldenTestConfigSbe;

/** SBE Value Equal to. */
class ValueEq : public Matcher {
public:
    explicit ValueEq(TypedValue v) : _v{v} {}

    std::string describe() const {
        std::stringstream ss;
        ss << "ValueEq(" << _v << ")";
        return ss.str();
    }

    MatchResult match(const TypedValue& x) const {
        auto [tag, val] = sbe::value::compareValue(_v.first, _v.second, x.first, x.second);
        return MatchResult{tag == sbe::value::TypeTags::NumberInt32 &&
                           sbe::value::bitcastTo<int>(val) == 0};
    }

private:
    TypedValue _v;
};

}  // namespace mongo::sbe
