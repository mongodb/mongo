// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {

/**
 * An implementation of the CollatorInterface used for testing that does not depend on the ICU
 * library.
 */
class CollatorInterfaceMock final : public CollatorInterface {
public:
    /**
     * The mock can compute a number of artificial collations. A test can request a particular
     * kind of mock collator and then assert that it obtains the mock behavior.
     */
    enum class MockType {
        // Compares strings after reversing them. For example, the comparison key for "abc" is
        // "cba".
        kReverseString,

        // Considers all strings equal.
        kAlwaysEqual,

        // Compares strings after converting them to lower case.  For example, the comparison key
        // for "FOO" is "foo".
        kToLowerString,
    };

    /**
     * Constructs a mock collator which computes the collation described by 'mockType'. The
     * collator's spec will have a fake locale string such as "mock_reverse".
     */
    CollatorInterfaceMock(MockType mockType);

    std::unique_ptr<CollatorInterface> clone() const final;
    std::shared_ptr<CollatorInterface> cloneShared() const final;

    int compare(std::string_view left, std::string_view right) const final;

    ComparisonKey getComparisonKey(std::string_view stringData) const final;

private:
    const MockType _mockType;
};

}  // namespace mongo
