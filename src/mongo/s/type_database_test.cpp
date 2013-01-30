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
#include "mongo/s/field_parser.h"
#include "mongo/s/type_database.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::DatabaseType;
    using mongo::FieldParser;
    using std::string;

    TEST(Validity, Empty) {
        DatabaseType db;
        BSONObj emptyObj = BSONObj();
        string errMsg;
        ASSERT_TRUE(db.parseBSON(emptyObj, &errMsg));
        ASSERT_FALSE(db.isValid(NULL));
    }

    TEST(Validity, BasicDatabase) {
        DatabaseType db;
        BSONObj obj = BSON(DatabaseType::name("mydb") <<
                           DatabaseType::primary("shard"));
        string errMsg;
        ASSERT_TRUE(db.parseBSON(obj, &errMsg));
        ASSERT_TRUE(db.isValid(NULL));
    }

    TEST(Compatibility, PartitionedIsIrrelevant) {
        DatabaseType db;
        BSONObj obj = BSON(DatabaseType::name("mydb") <<
                           DatabaseType::primary("shard") <<
                           DatabaseType::DEPRECATED_partitioned(true));
        string errMsg;
        ASSERT_TRUE(db.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(db.getName(), "mydb");
        ASSERT_EQUALS(db.getPrimary(), "shard");
        ASSERT_EQUALS(db.getDraining(), false);
    }

    TEST(Validity, BadType) {
        DatabaseType db;
        BSONObj obj = BSON(DatabaseType::name() << 0);
        string errMsg;
        ASSERT((!db.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

    TEST(Optionals, TestDefault) {
        DatabaseType dbNotDraining;
        BSONObj notDraining = BSON(DatabaseType::name("mydb") <<
                                   DatabaseType::primary("shard"));
        string errMsg;
        ASSERT_TRUE(dbNotDraining.parseBSON(notDraining, &errMsg));
        ASSERT_TRUE(dbNotDraining.isValid(NULL));
        ASSERT_TRUE(dbNotDraining.isDrainingSet());
        ASSERT_EQUALS(dbNotDraining.getDraining(), DatabaseType::draining.getDefault());
    }

    TEST(Optionals, TestSet) {
        DatabaseType dbDraining;
        BSONObj draining = BSON(DatabaseType::name("mydb") <<
                                DatabaseType::primary("shard") <<
                                DatabaseType::draining(true));
        string errMsg;
        ASSERT_TRUE(dbDraining.parseBSON(draining, &errMsg));
        ASSERT_TRUE(dbDraining.isValid(NULL));
        ASSERT_TRUE(dbDraining.isDrainingSet());
        ASSERT_TRUE(dbDraining.getDraining());
    }

    TEST(Optionals, TestNotSet) {
        DatabaseType dbNotDraining;
        dbNotDraining.setName("mydb");
        dbNotDraining.setPrimary("shard");
        ASSERT_TRUE(dbNotDraining.isValid(NULL));
        ASSERT_TRUE(dbNotDraining.isDrainingSet());
        ASSERT_EQUALS(dbNotDraining.getDraining(), DatabaseType::draining.getDefault());
        bool isDraining;
        BSONObj genObj;
        ASSERT_EQUALS( FieldParser::extract( genObj, DatabaseType::draining, &isDraining ),
                       FieldParser::FIELD_DEFAULT );
    }

    TEST(Optionals, RoundTripOptionalOff) {
        DatabaseType dbNotDraining;
        BSONObj notDraining = BSON(DatabaseType::name("mydb") <<
                                   DatabaseType::primary("shard"));
        ASSERT_TRUE(notDraining[DatabaseType::name()].ok());
        ASSERT_TRUE(notDraining[DatabaseType::primary()].ok());
        ASSERT_TRUE(notDraining[DatabaseType::draining()].eoo());
        string errMsg;
        ASSERT_TRUE(dbNotDraining.parseBSON(notDraining, &errMsg));
        ASSERT_EQUALS(dbNotDraining.toBSON(), notDraining);
    }

    TEST(Optionals, RoundTripOptionalOn) {
        DatabaseType dbDraining;
        BSONObj draining = BSON(DatabaseType::name("mydb") <<
                                DatabaseType::primary("shard") <<
                                DatabaseType::draining(true));
        ASSERT_TRUE(draining[DatabaseType::name()].ok());
        ASSERT_TRUE(draining[DatabaseType::primary()].ok());
        ASSERT_TRUE(draining[DatabaseType::draining()].ok());
        string errMsg;
        ASSERT_TRUE(dbDraining.parseBSON(draining, &errMsg));
        ASSERT_EQUALS(dbDraining.toBSON(), draining);
    }

} // unnamed namespace
