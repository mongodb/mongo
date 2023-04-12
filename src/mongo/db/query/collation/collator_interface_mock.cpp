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

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collator_interface_mock.h"

#include <algorithm>
#include <memory>
#include <string>

#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

std::string mockTypeToString(CollatorInterfaceMock::MockType type) {
    switch (type) {
        case CollatorInterfaceMock::MockType::kReverseString:
            return "mock_reverse_string";
        case CollatorInterfaceMock::MockType::kAlwaysEqual:
            return "mock_always_equal";
        case CollatorInterfaceMock::MockType::kToLowerString:
            return "mock_to_lower_string";
    }

    MONGO_UNREACHABLE;
}

Collation makeCollation(StringData locale, StringData version) {
    Collation collation(locale.toString());
    // "backwards" is optional. The ICU collator always sets it to true/false based on the locale.
    collation.setBackwards(false);
    collation.setVersion(version);
    return collation;
}

}  // namespace

CollatorInterfaceMock::CollatorInterfaceMock(MockType mockType)
    : CollatorInterface(makeCollation(mockTypeToString(mockType), "mock_version")),
      _mockType(mockType) {}

std::unique_ptr<CollatorInterface> CollatorInterfaceMock::clone() const {
    return std::make_unique<CollatorInterfaceMock>(_mockType);
}

std::shared_ptr<CollatorInterface> CollatorInterfaceMock::cloneShared() const {
    return std::make_shared<CollatorInterfaceMock>(_mockType);
}

int CollatorInterfaceMock::compare(StringData left, StringData right) const {
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
        case MockType::kToLowerString:
            return str::toLower(left).compare(str::toLower(right));
        case MockType::kAlwaysEqual:
            return 0;
    }

    MONGO_UNREACHABLE;
}

CollatorInterface::ComparisonKey CollatorInterfaceMock::getComparisonKey(
    StringData stringData) const {
    switch (_mockType) {
        case MockType::kReverseString: {
            std::string keyDataString = stringData.toString();
            std::reverse(keyDataString.begin(), keyDataString.end());
            return makeComparisonKey(std::move(keyDataString));
        }
        case MockType::kToLowerString:
            return makeComparisonKey(str::toLower(stringData));
        case MockType::kAlwaysEqual:
            return makeComparisonKey("always_equal");
    }

    MONGO_UNREACHABLE;
}

}  // namespace mongo
