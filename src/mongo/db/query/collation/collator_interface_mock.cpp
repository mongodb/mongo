// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collator_interface_mock.h"

#include "mongo/db/basic_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>


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

Collation makeCollation(std::string_view locale, std::string_view version) {
    Collation collation(std::string{locale});
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

int CollatorInterfaceMock::compare(std::string_view left, std::string_view right) const {
    switch (_mockType) {
        case MockType::kReverseString: {
            std::string leftString = std::string{left};
            std::string rightString = std::string{right};
            std::reverse(leftString.begin(), leftString.end());
            std::reverse(rightString.begin(), rightString.end());
            std::string_view leftReversed(leftString);
            std::string_view rightReversed(rightString);
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
    std::string_view stringData) const {
    switch (_mockType) {
        case MockType::kReverseString: {
            std::string keyDataString = std::string{stringData};
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
