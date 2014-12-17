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

#include "mongo/s/type_shard.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;
    using mongo::ShardType;
    using mongo::BSONObj;

    TEST(Validity, Empty) {
        ShardType shard;
        BSONObj emptyObj = BSONObj();
        string errMsg;
        ASSERT(shard.parseBSON(emptyObj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(shard.isValid(NULL));
    }

    TEST(Validity, MissingName) {
        ShardType shard;
        BSONObj obj = BSON(ShardType::host("localhost:27017"));
        string errMsg;
        ASSERT(shard.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(shard.isValid(NULL));
    }

    TEST(Validity, MissingHost) {
        ShardType shard;
        BSONObj obj = BSON(ShardType::name("shard0000"));
        string errMsg;
        ASSERT(shard.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(shard.isValid(NULL));
    }

    TEST(Validity, OnlyMandatory) {
        ShardType shard;
        BSONObj obj = BSON(ShardType::name("shard0000") <<
                           ShardType::host("localhost:27017"));
        string errMsg;
        ASSERT(shard.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(shard.isValid(NULL));
    }

    TEST(Validity, AllOptionalsPresent) {
        ShardType shard;
        BSONObj obj = BSON(ShardType::name("shard0000") <<
                           ShardType::host("localhost:27017") <<
                           ShardType::draining(true) <<
                           ShardType::maxSize(100));
        string errMsg;
        ASSERT(shard.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(shard.isValid(NULL));
    }

    TEST(Validity, MaxSizeAsFloat) {
        ShardType shard;
        BSONObj obj = BSON(ShardType::name("shard0000") <<
                           ShardType::host("localhost:27017") <<
                           ShardType::maxSize() << 100.0);
        string errMsg;
        ASSERT(shard.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(shard.isValid(NULL));
    }

    TEST(Validity, BadType) {
        ShardType shard;
        BSONObj obj = BSON(ShardType::name() << 0);
        string errMsg;
        ASSERT((!shard.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

}  // unnamed namespace
