/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/bson/oid.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

using namespace mongo;

using unittest::assertGet;

TEST(CollectionType, Empty) {
    StatusWith<CollectionType> status = CollectionType::fromBSON(BSONObj());
    ASSERT_FALSE(status.isOK());
}

TEST(CollectionType, Basic) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status =
        CollectionType::fromBSON(BSON(CollectionType::fullNs("db.coll")
                                      << CollectionType::epoch(oid)
                                      << CollectionType::updatedAt(Date_t::fromMillisSinceEpoch(1))
                                      << CollectionType::keyPattern(BSON("a" << 1))
                                      << CollectionType::unique(true)));
    ASSERT_TRUE(status.isOK());

    CollectionType coll = status.getValue();
    ASSERT_TRUE(coll.validate().isOK());
    ASSERT(coll.getNs() == NamespaceString{"db.coll"});
    ASSERT_EQUALS(coll.getEpoch(), oid);
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_EQUALS(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_EQUALS(coll.getUnique(), true);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
    ASSERT_EQUALS(coll.getDropped(), false);
}

TEST(CollectionType, EpochCorrectness) {
    CollectionType coll;
    coll.setNs(NamespaceString{"db.coll"});
    coll.setUpdatedAt(Date_t::fromMillisSinceEpoch(1));
    coll.setKeyPattern(KeyPattern{BSON("a" << 1)});
    coll.setUnique(false);
    coll.setDropped(false);

    // Validation will fail because we don't have epoch set. This ensures that if we read a
    // collection with no epoch, we will write back one with epoch.
    ASSERT_NOT_OK(coll.validate());

    // We should be allowed to set empty epoch for dropped collections
    coll.setDropped(true);
    coll.setEpoch(OID());
    ASSERT_OK(coll.validate());

    // We should be allowed to set normal epoch for non-dropped collections
    coll.setDropped(false);
    coll.setEpoch(OID::gen());
    ASSERT_OK(coll.validate());
}

TEST(CollectionType, Pre22Format) {
    CollectionType coll = assertGet(CollectionType::fromBSON(BSON("_id"
                                                                  << "db.coll"
                                                                  << "lastmod"
                                                                  << Date_t::fromMillisSinceEpoch(1)
                                                                  << "dropped"
                                                                  << false
                                                                  << "key"
                                                                  << BSON("a" << 1)
                                                                  << "unique"
                                                                  << false)));

    ASSERT(coll.getNs() == NamespaceString{"db.coll"});
    ASSERT(!coll.getEpoch().isSet());
    ASSERT_EQUALS(coll.getUpdatedAt(), Date_t::fromMillisSinceEpoch(1));
    ASSERT_EQUALS(coll.getKeyPattern().toBSON(), BSON("a" << 1));
    ASSERT_EQUALS(coll.getUnique(), false);
    ASSERT_EQUALS(coll.getAllowBalance(), true);
    ASSERT_EQUALS(coll.getDropped(), false);
}

TEST(CollectionType, InvalidCollectionNamespace) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> result =
        CollectionType::fromBSON(BSON(CollectionType::fullNs("foo\\bar.coll")
                                      << CollectionType::epoch(oid)
                                      << CollectionType::updatedAt(Date_t::fromMillisSinceEpoch(1))
                                      << CollectionType::keyPattern(BSON("a" << 1))
                                      << CollectionType::unique(true)));
    ASSERT_TRUE(result.isOK());
    CollectionType collType = result.getValue();
    ASSERT_FALSE(collType.validate().isOK());
}

TEST(CollectionType, BadType) {
    const OID oid = OID::gen();
    StatusWith<CollectionType> status = CollectionType::fromBSON(
        BSON(CollectionType::fullNs() << 1 << CollectionType::epoch(oid)
                                      << CollectionType::updatedAt(Date_t::fromMillisSinceEpoch(1))
                                      << CollectionType::keyPattern(BSON("a" << 1))
                                      << CollectionType::unique(true)));

    ASSERT_FALSE(status.isOK());
}

}  // namespace
