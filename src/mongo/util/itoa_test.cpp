/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <array>
#include <cstdint>
#include <limits>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/itoa.h"

namespace {
using namespace mongo;

TEST(ItoA, StringDataEquality) {
    for (auto testCase : {1u,
                          12u,
                          133u,
                          1446u,
                          17789u,
                          192923u,
                          2389489u,
                          29313479u,
                          1928127389u,
                          std::numeric_limits<std::uint32_t>::max()}) {
        ItoA itoa{testCase};
        ASSERT_EQ(std::to_string(testCase), StringData(itoa));
    }
}

}  // namespace
