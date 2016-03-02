/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/query/collation/collator_interface_mock.h"

#include <string>

#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

std::string mockTypeToString(CollatorInterfaceMock::MockType type) {
    switch (type) {
        case CollatorInterfaceMock::MockType::kReverseString:
            return "mock_reverse_string";
        case CollatorInterfaceMock::MockType::kAlwaysEqual:
            return "mock_always_equal";
    }

    MONGO_UNREACHABLE;
}

}  // namespace

CollatorInterfaceMock::CollatorInterfaceMock(MockType mockType)
    : CollatorInterface(CollationSpec(mockTypeToString(mockType))), _mockType(mockType) {}

int CollatorInterfaceMock::compare(StringData left, StringData right) {
    switch (_mockType) {
        case MockType::kReverseString: {
            std::string leftString = left.toString();
            std::string rightString = right.toString();
            std::reverse(leftString.begin(), leftString.end());
            std::reverse(rightString.begin(), rightString.end());
            StringData leftReversed(leftString);
            StringData rightReversed(rightString);
            return leftReversed.compare(rightReversed);
        }
        case MockType::kAlwaysEqual:
            return 0;
    }

    MONGO_UNREACHABLE;
}

CollatorInterface::ComparisonKey CollatorInterfaceMock::getComparisonKey(StringData stringData) {
    switch (_mockType) {
        case MockType::kReverseString: {
            std::string keyDataString = stringData.toString();
            std::reverse(keyDataString.begin(), keyDataString.end());
            return makeComparisonKey(std::move(keyDataString));
        }
        case MockType::kAlwaysEqual:
            return makeComparisonKey("always_equal");
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
