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

#include "mongo/bson/oid.h"
#include "mongo/s/type_locks.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::BSONObj;
    using mongo::LocksType;
    using mongo::OID;

    TEST(Validity, Empty) {
        LocksType lock;
        BSONObj emptyObj = BSONObj();
        lock.parseBSON(emptyObj);
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
        lock.parseBSON(obj);
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
        lock.parseBSON(obj);
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
        lock.parseBSON(obj);
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
        lock.parseBSON(obj);
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, LockedMissingLockID) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(2) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        lock.parseBSON(obj);
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, LockedMissingWho) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(2) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::why("doing balance round"));
        lock.parseBSON(obj);
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, LockedMissingWhy) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(2) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249"));
        lock.parseBSON(obj);
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
        lock.parseBSON(obj);
        ASSERT_TRUE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingProcess) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::state(1) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        lock.parseBSON(obj);
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingLockID) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(1) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249") <<
                           LocksType::why("doing balance round"));
        lock.parseBSON(obj);
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingWho) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(1) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::why("doing balance round"));
        lock.parseBSON(obj);
        ASSERT_FALSE(lock.isValid(NULL));
    }

    TEST(Validity, ContestedMissingWhy) {
        LocksType lock;
        BSONObj obj = BSON(LocksType::name("balancer") <<
                           LocksType::process("host.local:27017:1352918870:16807") <<
                           LocksType::state(1) <<
                           LocksType::lockID(OID::gen()) <<
                           LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249"));
        lock.parseBSON(obj);
        ASSERT_FALSE(lock.isValid(NULL));
    }
} // unnamed namespace
