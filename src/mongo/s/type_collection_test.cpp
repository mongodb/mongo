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
 */

#include "mongo/pch.h"

#include "mongo/bson/oid.h"
#include "mongo/bson/util/misc.h" // for Date_t
#include "mongo/s/type_collection.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::CollectionType;
    using mongo::BSONObj;
    using mongo::OID;
    using mongo::Date_t;

    TEST(Validity, Empty) {
        CollectionType coll;
        BSONObj emptyObj = BSONObj();
        coll.parseBSON(emptyObj);
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Validity, ShardedCollection) {
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::keyPattern(BSON("a" << 1)) <<
                           CollectionType::createdAt(1ULL) <<
                           CollectionType::epoch(OID::gen()));
        coll.parseBSON(obj);
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Validity, UnshardedCollection) {
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::primary("my_primary_shard") <<
                           CollectionType::createdAt(1ULL) <<
                           CollectionType::epoch(OID::gen()));
        coll.parseBSON(obj);
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Validity, MixingOptionals) {
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::createdAt(time(0)) <<
                           CollectionType::unique(true));
        coll.parseBSON(obj);
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Compatibility, OldLastmod ) {
        CollectionType coll;
        Date_t creation(time(0));
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::primary("my_primary_shard") <<
                           CollectionType::DEPRECATED_lastmod(creation) <<
                           CollectionType::epoch(OID::gen()));
        coll.parseBSON(obj);
        ASSERT_TRUE(coll.isValid(NULL));
        ASSERT_EQUALS(coll.getCreatedAt(), creation);
    }

    TEST(Compatibility, OldEpoch) {
        CollectionType coll;
        OID epoch = OID::gen();
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::primary("my_primary_shard") <<
                           CollectionType::createdAt(1ULL) <<
                           CollectionType::DEPRECATED_lastmodEpoch(epoch));
        coll.parseBSON(obj);
        ASSERT_TRUE(coll.isValid(NULL));
        ASSERT_EQUALS(coll.getEpoch(), epoch);
    }

    TEST(Compatibility, OldDroppedTrue) {
        // The 'dropped' field creates a special case. We'd parse the doc containing it but
        // would generate an empty CollectionType, which is not valid.
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::keyPattern(BSON("a" << 1)) <<
                           CollectionType::unique(false) <<
                           CollectionType::DEPRECATED_lastmod(1ULL) <<
                           CollectionType::DEPRECATED_lastmodEpoch(OID::gen()) <<
                           CollectionType::DEPRECATED_dropped(true));
        coll.parseBSON(obj);
        ASSERT_EQUALS(coll.getNS(), "");
        ASSERT_EQUALS(coll.getKeyPattern(), BSONObj());
        ASSERT_EQUALS(coll.getUnique(), false);
        ASSERT_EQUALS(coll.getCreatedAt(), 0ULL);
        ASSERT_EQUALS(coll.getEpoch(), OID());
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Compatibility, OldDroppedFalse) {
        CollectionType coll;
        OID epoch = OID::gen();
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::keyPattern(BSON("a" << 1)) <<
                           CollectionType::unique(true) <<
                           CollectionType::DEPRECATED_lastmod(1ULL) <<
                           CollectionType::DEPRECATED_lastmodEpoch(epoch) <<
                           CollectionType::DEPRECATED_dropped(false));
        coll.parseBSON(obj);
        ASSERT_EQUALS(coll.getNS(), "db.coll");
        ASSERT_EQUALS(coll.getKeyPattern(), BSON("a" << 1));
        ASSERT_EQUALS(coll.getUnique(), true);
        ASSERT_EQUALS(coll.getCreatedAt(), 1ULL);
        ASSERT_EQUALS(coll.getEpoch(), epoch);
        ASSERT_TRUE(coll.isValid(NULL));
    }

} // unnamed namespace
