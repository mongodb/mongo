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
#include "mongo/s/type_database.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::DatabaseType;
    using mongo::BSONObj;

    TEST(Validity, Empty) {
        DatabaseType db;
        BSONObj emptyObj = BSONObj();
        db.parseBSON(emptyObj);
        ASSERT_FALSE(db.isValid(NULL));
    }

    TEST(Validity, NonScatteredDatabase) {
        DatabaseType db;
        BSONObj obj = BSON(DatabaseType::name("mydb") <<
                           DatabaseType::primary("shard"));
        db.parseBSON(obj);
        ASSERT_TRUE(db.isValid(NULL));
    }

    TEST(Validity, ScatteredDatabase) {
        DatabaseType db;
        BSONObj obj = BSON(DatabaseType::name("mydb") <<
                           DatabaseType::primary("shard") <<
                           DatabaseType::scattered(true));
        db.parseBSON(obj);
        ASSERT_TRUE(db.isValid(NULL));
    }

    TEST(Compatibility, PartitionedIsIrrelevant) {
        DatabaseType db;
        BSONObj obj = BSON(DatabaseType::name("mydb") <<
                           DatabaseType::primary("shard") <<
                           DatabaseType::DEPRECATED_partitioned(true));
        db.parseBSON(obj);
        ASSERT_EQUALS(db.getName(), "mydb");
        ASSERT_EQUALS(db.getPrimary(), "shard");
        ASSERT_EQUALS(db.getScattered(), false);
        ASSERT_EQUALS(db.getDraining(), false);
    }

} // unnamed namespace
