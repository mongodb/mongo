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

#include "mongo/bson/oid.h"
#include "mongo/s/type_locks.h"
#include "mongo/unittest/unittest.h"

namespace {

    using std::string;
    using mongo::BSONObj;
    using mongo::LocksType;
    using mongo::OID;

    TEST(Validity, Empty) {
        LocksType lock;
        BSONObj emptyObj = BSONObj();
        string errMsg;
        ASSERT(lock.parseBSON(emptyObj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, UnlockedWithOptional) {
        LocksType lock;
        OID testLockID = OID::gen();
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(0) <<
                           LocksType::lockID(testLockID) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(lock.isValid(NULL));
        ASSERT_EQUALS(lock.getName(), "balancer");
        ASSERT_EQUALS(lock.getProcess(), "host.local:27017:1352918870:16807");
        ASSERT_EQUALS(lock.getState(), 0);
        ASSERT_EQUALS(lock.getLockID(), testLockID);
        ASSERT_EQUALS(lock.getWho(), "host.local:27017:1352918870:16807:Balancer:282475249");
        ASSERT_EQUALS(lock.getWhy(), "doing balance round");
    }

    TEST(Validity, UnlockedWithoutOptional) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::state(0));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(lock.isValid(NULL));
        ASSERT_EQUALS(lock.getName(), "balancer");
        ASSERT_EQUALS(lock.getState(), 0);
    }

    TEST(Validity, LockedValid) {
        LocksType lock;
        OID testLockID = OID::gen();
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(2) <<
                           LocksType::lockID(testLockID) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(lock.isValid(NULL));
        ASSERT_EQUALS(lock.getName(), "balancer");
        ASSERT_EQUALS(lock.getProcess(), "host.local:27017:1352918870:16807");
        ASSERT_EQUALS(lock.getState(), 2);
        ASSERT_EQUALS(lock.getLockID(), testLockID);
        ASSERT_EQUALS(lock.getWho(), "host.local:27017:1352918870:16807:Balancer:282475249");
        ASSERT_EQUALS(lock.getWhy(), "doing balance round");
    }

    TEST(Validity, LockedMissingProcess) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::state(2) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, LockedMissingLockID) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(2) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, LockedMissingWho) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(2) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, LockedMissingWhy) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(2) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedValid) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(1) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_TRUE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingProcess) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::state(1) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingLockID) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(1) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingWho) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(1) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::why("doing balance round"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingWhy) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(1) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249"));
        string errMsg;
        ASSERT(lock.parseBSON(obj, &errMsg));
        ASSERT_EQUALS(errMsg, "");
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, BadType) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name() << 0);
        string errMsg;
        ASSERT((!lock.parseBSON(obj, &errMsg)) && (errMsg != ""));
    }

} // unnamed namespace
