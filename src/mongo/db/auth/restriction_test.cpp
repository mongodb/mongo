// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/restriction.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/auth/restriction_environment.h"
#include "mongo/db/auth/restriction_mock.h"
#include "mongo/db/auth/restriction_set.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/sockaddr.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo {

using namespace restriction_detail;

class RestrictionSetTestWithSession : public unittest::Test {
public:
    const transport::Session& getSession() {
        return _session;
    }

private:
    transport::MockSession _session = transport::MockSession(HostAndPort(), {}, {}, nullptr);
};

TEST_F(RestrictionSetTestWithSession, EmptyRestrictionSetAllValidates) {
    auto env = getSession().getAuthEnvironment();
    RestrictionSetAll<UnnamedRestriction> set;
    Status status = set.validate(env);
    ASSERT_OK(status);
}

TEST_F(RestrictionSetTestWithSession, RestrictionSetAllWithMetRestrictionValidates) {
    auto env = getSession().getAuthEnvironment();
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(true));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST_F(RestrictionSetTestWithSession, RestrictionSetAllWithMetRestrictionsValidates) {
    auto env = getSession().getAuthEnvironment();
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(true));
    restrictions.push_back(std::make_unique<RestrictionMock>(true));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST_F(RestrictionSetTestWithSession, RestrictionSetAllWithFailedRestrictionFails) {
    auto env = getSession().getAuthEnvironment();
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(false));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST_F(RestrictionSetTestWithSession, RestrictionSetAllWithMetAndUnmetRestrictionsFails) {
    auto env = getSession().getAuthEnvironment();
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(true));
    restrictions.push_back(std::make_unique<RestrictionMock>(false));
    RestrictionSetAll<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST_F(RestrictionSetTestWithSession, EmptyRestrictionSetAnyValidates) {
    auto env = getSession().getAuthEnvironment();
    RestrictionSetAny<UnnamedRestriction> set;
    Status status = set.validate(env);
    ASSERT_OK(status);
}

TEST_F(RestrictionSetTestWithSession, RestrictionSetAnyWithMetRestrictionValidates) {
    auto env = getSession().getAuthEnvironment();
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(true));
    RestrictionSetAny<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_OK(set.validate(env));
}

TEST_F(RestrictionSetTestWithSession, RestrictionSetAnyWithFailedRestrictionFails) {
    auto env = getSession().getAuthEnvironment();
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(false));
    RestrictionSetAny<UnnamedRestriction> set(std::move(restrictions));
    ASSERT_NOT_OK(set.validate(env));
}

TEST_F(RestrictionSetTestWithSession, RestrictionSetAnyWithMetAndUnmetRestrictionsValidates) {
    auto env = getSession().getAuthEnvironment();
    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(true));
    restrictions.push_back(std::make_unique<RestrictionMock>(false));
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
    restrictions.push_back(std::make_unique<NamedRestrictionMock>("a", true));
    restrictions.push_back(std::make_unique<NamedRestrictionMock>("b", false));
    restrictions.push_back(std::make_unique<NamedRestrictionMock>("c", true));
    RestrictionSet<> mockSet(std::move(restrictions));
    ASSERT_BSONOBJ_EQ(mockSet.toBSON(), BSON("a" << true << "b" << false << "c" << true));
}

TEST(RestrictionSetTest, SerializeRestrictionSetAnyToBSON) {
    RestrictionSetAny<UnnamedRestriction> emptySet;
    ASSERT_BSONOBJ_EQ(emptySet.toBSON(), BSONArray());

    std::vector<std::unique_ptr<UnnamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<RestrictionMock>(true));
    restrictions.push_back(std::make_unique<RestrictionMock>(false));
    restrictions.push_back(std::make_unique<RestrictionMock>(true));

    RestrictionSetAny<UnnamedRestriction> mockSet(std::move(restrictions));
    ASSERT_BSONOBJ_EQ(mockSet.toBSON(), BSON_ARRAY(true << false << true));
}

TEST(RestrictionSetTest, SerializeRestrictionDocumentToBSON) {
    RestrictionDocument<> emptyDoc;
    ASSERT_BSONOBJ_EQ(emptyDoc.toBSON(), BSONArray());

    std::vector<std::unique_ptr<NamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<NamedRestrictionMock>("truthy", true));
    restrictions.push_back(std::make_unique<NamedRestrictionMock>("falsey", false));
    auto mockSet = std::make_unique<RestrictionSet<>>(std::move(restrictions));
    RestrictionDocument<> mockDoc(std::move(mockSet));
    ASSERT_BSONOBJ_EQ(mockDoc.toBSON(), BSON_ARRAY(BSON("truthy" << true << "falsey" << false)));
}

TEST(RestrictionSetTest, SerializeRestrictionDocumentsToBSON) {
    RestrictionDocuments emptyDoc;
    ASSERT_BSONOBJ_EQ(emptyDoc.toBSON(), BSONArray());

    std::vector<std::unique_ptr<NamedRestriction>> restrictions;
    restrictions.push_back(std::make_unique<NamedRestrictionMock>("truthy", true));
    restrictions.push_back(std::make_unique<NamedRestrictionMock>("falsey", false));
    auto mockSet = std::make_unique<RestrictionSet<>>(std::move(restrictions));
    auto mockDoc = std::make_unique<RestrictionDocument<>>(std::move(mockSet));
    RestrictionDocuments mockDocs(std::move(mockDoc));
    ASSERT_BSONOBJ_EQ(mockDocs.toBSON(),
                      BSON_ARRAY(BSON_ARRAY(BSON("truthy" << true << "falsey" << false))));
}

}  // namespace mongo
