/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/platform/basic.h"

#include <array>
#include <cstdint>
#include <limits>

#include "merizo/base/string_data.h"
#include "merizo/unittest/unittest.h"
#include "merizo/util/itoa.h"

namespace {
using namespace merizo;

TEST(ItoA, StringDataEquality) {
    ASSERT_EQ(ItoA::kBufSize - 1, std::to_string(std::numeric_limits<std::uint64_t>::max()).size());

    for (auto testCase : {uint64_t(1),
                          uint64_t(12),
                          uint64_t(133),
                          uint64_t(1446),
                          uint64_t(17789),
                          uint64_t(192923),
                          uint64_t(2389489),
                          uint64_t(29313479),
                          uint64_t(1928127389),
                          std::numeric_limits<std::uint64_t>::max() - 1,
                          std::numeric_limits<std::uint64_t>::max()}) {
        ItoA itoa{testCase};
        ASSERT_EQ(std::to_string(testCase), StringData(itoa));
    }
}

}  // namespace
