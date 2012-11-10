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

    TEST(Validity, Empty) {
        mongo::CollectionType coll;
        coll.parseBSON(mongo::BSONObj());
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Validity, ShardedCollection) {
        mongo::CollectionType coll;
        coll.parseBSON(BSON(mongo::CollectionType::ns("db.coll") <<
                            mongo::CollectionType::keyPattern(BSON("a" << 1)) <<
                            mongo::CollectionType::createdAt(1ULL) <<
                            mongo::CollectionType::epoch(mongo::OID::gen())));
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Validity, UnshardedCollection) {
        mongo::CollectionType coll;
        coll.parseBSON(BSON(mongo::CollectionType::ns("db.coll") <<
                            mongo::CollectionType::primary("my_primary_shard") <<
                            mongo::CollectionType::createdAt(1ULL) <<
                            mongo::CollectionType::epoch(mongo::OID::gen())));
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Validity, MixingOptionals) {
        mongo::CollectionType coll;
        coll.parseBSON(BSON(mongo::CollectionType::ns("db.coll") <<
                            mongo::CollectionType::createdAt(time(0)) <<
                            mongo::CollectionType::unique(true)));
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Compatibility, OldLastmod ) {
        mongo::CollectionType coll;
        mongo::Date_t creation(time(0));
        coll.parseBSON(BSON(mongo::CollectionType::ns("db.coll") <<
                            mongo::CollectionType::primary("my_primary_shard") <<
                            mongo::CollectionType::DEPRECATED_lastmod(creation) <<
                            mongo::CollectionType::epoch(mongo::OID::gen())));
        ASSERT_TRUE(coll.isValid(NULL));
        ASSERT_EQUALS(coll.getCreatedAt(), creation);
    }

    TEST(Compatibility, OldEpoch) {
        mongo::CollectionType coll;
        mongo::OID epoch = mongo::OID::gen();
        coll.parseBSON(BSON(mongo::CollectionType::ns("db.coll") <<
                            mongo::CollectionType::primary("my_primary_shard") <<
                            mongo::CollectionType::createdAt(1ULL) <<
                            mongo::CollectionType::DEPRECATED_lastmodEpoch(epoch)));
        ASSERT_TRUE(coll.isValid(NULL));
        ASSERT_EQUALS(coll.getEpoch(), epoch);
    }

    TEST(Compatibility, OldDroppedTrue) {
        // The 'dropped' field creates a special case. We'd parse the doc containing it but
        // would generate and empty CollectionType, which is not valid.
        mongo::CollectionType coll;
        coll.parseBSON(BSON(mongo::CollectionType::ns("db.coll") <<
                            mongo::CollectionType::keyPattern(BSON("a" << 1)) <<
                            mongo::CollectionType::unique(false) <<
                            mongo::CollectionType::DEPRECATED_lastmod(1ULL) <<
                            mongo::CollectionType::DEPRECATED_lastmodEpoch(mongo::OID::gen()) <<
                            mongo::CollectionType::DEPRECATED_dropped(true)));
        ASSERT_EQUALS(coll.getNS(), "");
        ASSERT_EQUALS(coll.getKeyPattern(), mongo::BSONObj());
        ASSERT_EQUALS(coll.getUnique(), false);
        ASSERT_EQUALS(coll.getCreatedAt(), 0ULL);
        ASSERT_EQUALS(coll.getEpoch(), mongo::OID());
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Compatibility, OldDroppedFalse) {
        mongo::CollectionType coll;
        mongo::OID epoch = mongo::OID::gen();
        coll.parseBSON(BSON(mongo::CollectionType::ns("db.coll") <<
                            mongo::CollectionType::keyPattern(BSON("a" << 1)) <<
                            mongo::CollectionType::unique(true) <<
                            mongo::CollectionType::DEPRECATED_lastmod(1ULL) <<
                            mongo::CollectionType::DEPRECATED_lastmodEpoch(epoch) <<
                            mongo::CollectionType::DEPRECATED_dropped(false)));
        ASSERT_EQUALS(coll.getNS(), "db.coll");
        ASSERT_EQUALS(coll.getKeyPattern(), BSON("a" << 1));
        ASSERT_EQUALS(coll.getUnique(), true);
        ASSERT_EQUALS(coll.getCreatedAt(), 1ULL);
        ASSERT_EQUALS(coll.getEpoch(), epoch);
        ASSERT_TRUE(coll.isValid(NULL));
    }

} // unnamed namespace
