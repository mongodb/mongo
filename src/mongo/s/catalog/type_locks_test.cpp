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
#include "mongo/db/jsobj.h"
#include "mongo/s/catalog/type_locks.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

TEST(Validity, Empty) {
    BSONObj emptyObj = BSONObj();
    auto locksResult = LocksType::fromBSON(emptyObj);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, locksResult.getStatus());
}

TEST(Validity, UnlockedWithOptional) {
    OID testLockID = OID::gen();
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::process("host.local:27017:1352918870:16807")
                       << LocksType::state(LocksType::State::UNLOCKED)
                       << LocksType::lockID(testLockID)
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249")
                       << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();

    ASSERT_EQUALS(lock.getName(), "balancer");
    ASSERT_EQUALS(lock.getProcess(), "host.local:27017:1352918870:16807");
    ASSERT_EQUALS(lock.getState(), LocksType::State::UNLOCKED);
    ASSERT_EQUALS(lock.getLockID(), testLockID);
    ASSERT_EQUALS(lock.getWho(), "host.local:27017:1352918870:16807:Balancer:282475249");
    ASSERT_EQUALS(lock.getWhy(), "doing balance round");
}

TEST(Validity, UnlockedWithoutOptional) {
    BSONObj obj = BSON(LocksType::name("balancer") << LocksType::state(LocksType::State::UNLOCKED));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();

    ASSERT_EQUALS(lock.getName(), "balancer");
    ASSERT_EQUALS(lock.getState(), LocksType::State::UNLOCKED);
}

TEST(Validity, LockedValid) {
    OID testLockID = OID::gen();
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::process("host.local:27017:1352918870:16807")
                       << LocksType::state(LocksType::State::LOCKED)
                       << LocksType::lockID(testLockID)
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249")
                       << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();

    ASSERT_EQUALS(lock.getName(), "balancer");
    ASSERT_EQUALS(lock.getProcess(), "host.local:27017:1352918870:16807");
    ASSERT_EQUALS(lock.getState(), LocksType::State::LOCKED);
    ASSERT_EQUALS(lock.getLockID(), testLockID);
    ASSERT_EQUALS(lock.getWho(), "host.local:27017:1352918870:16807:Balancer:282475249");
    ASSERT_EQUALS(lock.getWhy(), "doing balance round");
}

TEST(Validity, LockedMissingProcess) {
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::state(LocksType::State::LOCKED)
                       << LocksType::lockID(OID::gen())
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249")
                       << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, LockedMissingLockID) {
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::process("host.local:27017:1352918870:16807")
                       << LocksType::state(LocksType::State::LOCKED)
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249")
                       << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, LockedMissingWho) {
    BSONObj obj =
        BSON(LocksType::name("balancer") << LocksType::process("host.local:27017:1352918870:16807")
                                         << LocksType::state(LocksType::State::LOCKED)
                                         << LocksType::lockID(OID::gen())
                                         << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, LockedMissingWhy) {
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::process("host.local:27017:1352918870:16807")
                       << LocksType::state(LocksType::State::LOCKED)
                       << LocksType::lockID(OID::gen())
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, ContestedValid) {
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::process("host.local:27017:1352918870:16807")
                       << LocksType::state(LocksType::State::LOCK_PREP)
                       << LocksType::lockID(OID::gen())
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249")
                       << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_OK(lock.validate());
}

TEST(Validity, ContestedMissingProcess) {
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::state(LocksType::State::LOCK_PREP)
                       << LocksType::lockID(OID::gen())
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249")
                       << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, ContestedMissingLockID) {
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::process("host.local:27017:1352918870:16807")
                       << LocksType::state(LocksType::State::LOCK_PREP)
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249")
                       << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, ContestedMissingWho) {
    BSONObj obj =
        BSON(LocksType::name("balancer") << LocksType::process("host.local:27017:1352918870:16807")
                                         << LocksType::state(LocksType::State::LOCK_PREP)
                                         << LocksType::lockID(OID::gen())
                                         << LocksType::why("doing balance round"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, ContestedMissingWhy) {
    BSONObj obj = BSON(LocksType::name("balancer")
                       << LocksType::process("host.local:27017:1352918870:16807")
                       << LocksType::state(LocksType::State::LOCK_PREP)
                       << LocksType::lockID(OID::gen())
                       << LocksType::who("host.local:27017:1352918870:16807:Balancer:282475249"));
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_OK(locksResult.getStatus());

    const LocksType& lock = locksResult.getValue();
    ASSERT_NOT_OK(lock.validate());
}

TEST(Validity, BadType) {
    BSONObj obj = BSON(LocksType::name() << 0);
    auto locksResult = LocksType::fromBSON(obj);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, locksResult.getStatus());
}

}  // unnamed namespace
