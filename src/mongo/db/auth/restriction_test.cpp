/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/auth/restriction.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/restriction_mock.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using namespace detail;

TEST(RestrictionSetTest, EmptyRestrictionSetAllValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    RestrictionSetAll<Restriction> set;
    Status status = set.validate(env);
    ASSERT_OK(status);
}

TEST(RestrictionSetTest, RestrictionSetAllWithMetRestrictionValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<Restriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    RestrictionSetAll<Restriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAllWithMetRestrictionsValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<Restriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    RestrictionSetAll<Restriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAllWithFailedRestrictionFails) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<Restriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAll<Restriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAllWithMetAndUnmetRestrictionsFails) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<Restriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAll<Restriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST(RestrictionSetTest, EmptyRestrictionSetAnyValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    RestrictionSetAny<Restriction> set;
    Status status = set.validate(env);
    ASSERT_OK(status);
}

TEST(RestrictionSetTest, RestrictionSetAnyWithMetRestrictionValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<Restriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    RestrictionSetAny<Restriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAnyWithFailedRestrictionFails) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<Restriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAny<Restriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAnyWithMetAndUnmetRestrictionsValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<Restriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAny<Restriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}


}  // namespace mongo
