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

using namespace restriction_detail;

TEST(RestrictionSetTest, EmptyRestrictionSetAllValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    RestrictionSetAll<UnnamedRestriction> set;
    Status status = set.validate(env);
    ASSERT_OK(status);
}

TEST(RestrictionSetTest, RestrictionSetAllWithMetRestrictionValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAllWithMetRestrictionsValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAllWithFailedRestrictionFails) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAllWithMetAndUnmetRestrictionsFails) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST(RestrictionSetTest, EmptyRestrictionSetAnyValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    RestrictionSetAny<UnnamedRestriction> set;
    Status status = set.validate(env);
    ASSERT_OK(status);
}

TEST(RestrictionSetTest, RestrictionSetAnyWithMetRestrictionValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    RestrictionSetAny<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAnyWithFailedRestrictionFails) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAny<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST(RestrictionSetTest, RestrictionSetAnyWithMetAndUnmetRestrictionsValidates) {
    RestrictionEnvironment env{SockAddr(), SockAddr()};
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    RestrictionSetAny<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST(RestrictionSetTest, SerializeRestrictionToBSON) {
    const auto toArray = [](const UnnamedRestriction& r) {
        BSONArrayBuilder b;
        r.appendToBuilder(&b);
        return b.arr();
    };

    const auto toObject = [](const NamedRestriction& r) {
        BSONObjBuilder b;
        r.appendToBuilder(&b);
        return b.obj();
    };

    RestrictionMock truthy(true), falsey(false);
    NamedRestrictionMock nTruthy("truthy", true), nFalsey("falsey", false);
    ASSERT_BSONOBJ_EQ(toArray(truthy), BSON_ARRAY(true));
    ASSERT_BSONOBJ_EQ(toArray(falsey), BSON_ARRAY(false));
    ASSERT_BSONOBJ_EQ(toObject(nTruthy), BSON("truthy" << true));
    ASSERT_BSONOBJ_EQ(toObject(nFalsey), BSON("falsey" << false));
}

TEST(RestrictionSetTest, SerializeRestrictionSetToBSON) {
    RestrictionSet<> emptySet;
    ASSERT_BSONOBJ_EQ(emptySet.toBSON(), BSONObj());

    std::vector<std::unique_ptr<NamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<NamedRestrictionMock>("a", true));
    restrictions.push_back(stdx::make_unique<NamedRestrictionMock>("b", false));
    restrictions.push_back(stdx::make_unique<NamedRestrictionMock>("c", true));
    RestrictionSet<> mockSet(std::move(restrictions));
    ASSERT_BSONOBJ_EQ(mockSet.toBSON(), BSON("a" << true << "b" << false << "c" << true));
}

TEST(RestrictionSetTest, SerializeRestrictionSetAnyToBSON) {
    RestrictionSetAny<UnnamedRestriction> emptySet;
    ASSERT_BSONOBJ_EQ(emptySet.toBSON(), BSONArray());

    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(false));
    restrictions.push_back(stdx::make_unique<RestrictionMock>(true));

    RestrictionSetAny<UnnamedRestriction> mockSet(std::move(restrictions));
    ASSERT_BSONOBJ_EQ(mockSet.toBSON(), BSON_ARRAY(true << false << true));
}

TEST(RestrictionSetTest, SerializeRestrictionDocumentToBSON) {
    RestrictionDocument<> emptyDoc;
    ASSERT_BSONOBJ_EQ(emptyDoc.toBSON(), BSONArray());

    std::vector<std::unique_ptr<NamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<NamedRestrictionMock>("truthy", true));
    restrictions.push_back(stdx::make_unique<NamedRestrictionMock>("falsey", false));
    auto mockSet = stdx::make_unique<RestrictionSet<>>(std::move(restrictions));
    RestrictionDocument<> mockDoc(std::move(mockSet));
    ASSERT_BSONOBJ_EQ(mockDoc.toBSON(), BSON_ARRAY(BSON("truthy" << true << "falsey" << false)));
}

TEST(RestrictionSetTest, SerializeRestrictionDocumentsToBSON) {
    RestrictionDocuments emptyDoc;
    ASSERT_BSONOBJ_EQ(emptyDoc.toBSON(), BSONArray());

    std::vector<std::unique_ptr<NamedRestriction>> restrictions;
    restrictions.push_back(stdx::make_unique<NamedRestrictionMock>("truthy", true));
    restrictions.push_back(stdx::make_unique<NamedRestrictionMock>("falsey", false));
    auto mockSet = stdx::make_unique<RestrictionSet<>>(std::move(restrictions));
    auto mockDoc = stdx::make_unique<RestrictionDocument<>>(std::move(mockSet));
    RestrictionDocuments mockDocs(std::move(mockDoc));
    ASSERT_BSONOBJ_EQ(mockDocs.toBSON(),
                      BSON_ARRAY(BSON_ARRAY(BSON("truthy" << true << "falsey" << false))));
}

}  // namespace mongo
