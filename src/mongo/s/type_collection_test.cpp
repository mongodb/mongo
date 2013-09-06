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

#include "mongo/pch.h"

#include "mongo/bson/oid.h"
#include "mongo/s/type_collection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace {

    using std::string;
    using mongo::CollectionType;
    using mongo::BSONObj;
    using mongo::OID;
    using mongo::Date_t;

    TEST(Validity, Empty) {
        CollectionType coll;
        BSONObj emptyObj = BSONObj();
        string errMsg;
        ASSERT(coll.parseBSON(emptyObj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Validity, ShardedCollection) {
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::keyPattern(BSON("a" << 1)) <<
                           CollectionType::updatedAt(1ULL) <<
                           CollectionType::epoch(OID::gen()));
        string errMsg;
        ASSERT(coll.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Validity, UnshardedCollection) {
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::primary("my_primary_shard") <<
                           CollectionType::updatedAt(1ULL) <<
                           CollectionType::epoch(OID::gen()));
        string errMsg;
        ASSERT(coll.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Validity, MixingOptionals) {
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::updatedAt(time(0)) <<
                           CollectionType::unique(true));
        string errMsg;
        ASSERT(coll.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(coll.isValid(NULL));
    }

    TEST(Compatibility, OldLastmod ) {
        CollectionType coll;
        Date_t creation(time(0));
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::primary("my_primary_shard") <<
                           CollectionType::DEPRECATED_lastmod(creation) <<
                           CollectionType::epoch(OID::gen()));
        string errMsg;
        ASSERT(coll.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(coll.isValid(NULL));
        ASSERT_EQUALS(coll.getUpdatedAt(), creation);
    }

    TEST(Compatibility, OldEpoch) {
        CollectionType coll;
        OID epoch = OID::gen();
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::primary("my_primary_shard") <<
                           CollectionType::updatedAt(1ULL) <<
                           CollectionType::DEPRECATED_lastmodEpoch(epoch));
        string errMsg;
        ASSERT(coll.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(coll.isValid(NULL));
        ASSERT_EQUALS(coll.getEpoch(), epoch);
    }

    TEST(Compatibility, OldDroppedTrue) {
        // The 'dropped' field creates a special case. We still validly parse the document
        // containing it but we need to ignore dropped collections in code which uses this.
        // Dropped collections should not have sharding information.
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::DEPRECATED_lastmod(1ULL) <<
                           CollectionType::DEPRECATED_lastmodEpoch(OID::gen()) <<
                           CollectionType::dropped(true));
        string errMsg;
        ASSERT(coll.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Compatibility, OldDroppedFalse) {
        CollectionType coll;
        OID epoch = OID::gen();
        BSONObj obj = BSON(CollectionType::ns("db.coll") <<
                           CollectionType::keyPattern(BSON("a" << 1)) <<
                           CollectionType::unique(true) <<
                           CollectionType::DEPRECATED_lastmod(1ULL) <<
                           CollectionType::DEPRECATED_lastmodEpoch(epoch) <<
                           CollectionType::dropped(false));
        string errMsg;
        ASSERT(coll.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_EQUALS(coll.getNS(), "db.coll");
        ASSERT_EQUALS(coll.getKeyPattern(), BSON("a" << 1));
        ASSERT_EQUALS(coll.getUnique(), true);
        ASSERT_EQUALS(coll.getUpdatedAt(), 1ULL);
        ASSERT_EQUALS(coll.getEpoch(), epoch);
        ASSERT_TRUE(coll.isValid(NULL));
    }

    TEST(Validity, BadType) {
        CollectionType coll;
        BSONObj obj = BSON(CollectionType::ns() << 0);
        string errMsg;
        ASSERT((!coll.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

} // unnamed namespace
