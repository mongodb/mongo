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
#include "mongo/db/exec/sbe/values/value_printer.h"
#include "mongo/db/exec/sbe/vm/vm_printer.h"
#include <sstream>

namespace mongo::sbe {

using namespace mongo::unittest;
using namespace mongo::unittest::match;

typedef std::pair<value::TypeTags, value::Value> TypedValue;

extern unittest::GoldenTestConfig goldenTestConfigSbe;

class SBETestFixture : public virtual mongo::unittest::Test {
protected:
    template <typename Stream>
    value::ValuePrinter<Stream> makeValuePrinter(Stream& stream) {
        return value::ValuePrinters::make(
            stream, PrintOptions().useTagForAmbiguousValues(true).normalizeOutput(true));
    }

    vm::CodeFragmentPrinter makeCodeFragmentPrinter() {
        return vm::CodeFragmentPrinter(vm::CodeFragmentPrinter::PrintFormat::Stable);
    }
};

class GoldenSBETestFixture : public virtual SBETestFixture {
public:
    GoldenSBETestFixture(bool debug = false) : _debug(debug), _variationCount(0) {}

    void run();
    void printVariation(const std::string& name = "");

protected:
    GoldenTestContext* gctx;

private:
    bool _debug;
    int _variationCount;
};

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

class ValueVectorGuard {
    ValueVectorGuard() = delete;
    ValueVectorGuard& operator=(const ValueVectorGuard&) = delete;

public:
    ValueVectorGuard(std::vector<TypedValue>& values) : _values(values) {}

    ~ValueVectorGuard() {
        for (auto p : _values) {
            value::releaseValue(p.first, p.second);
        }
    }

private:
    std::vector<TypedValue>& _values;
};

}  // namespace mongo::sbe
