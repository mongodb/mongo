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

#pragma once

#include "mongo/db/query/collation/collator_interface.h"

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

    int compare(StringData left, StringData right) const final;

    ComparisonKey getComparisonKey(StringData stringData) const final;

private:
    const MockType _mockType;
};

}  // namespace mongo
