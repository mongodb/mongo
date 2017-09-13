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

#include "mongo/s/catalog/type_locks.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
const std::string LocksType::ConfigNS = "config.locks";

const BSONField<std::string> LocksType::name("_id");
const BSONField<LocksType::State> LocksType::state("state");
const BSONField<std::string> LocksType::process("process");
const BSONField<OID> LocksType::lockID("ts");
const BSONField<std::string> LocksType::who("who");
const BSONField<std::string> LocksType::why("why");
const BSONField<Date_t> LocksType::when("when");

StatusWith<LocksType> LocksType::fromBSON(const BSONObj& source) {
    LocksType lock;

    {
        std::string lockName;
        Status status = bsonExtractStringField(source, name.name(), &lockName);
        if (!status.isOK())
            return status;
        lock._name = lockName;
    }

    {
        long long lockStateInt;
        Status status = bsonExtractIntegerField(source, state.name(), &lockStateInt);
        if (!status.isOK())
            return status;
        lock._state = static_cast<State>(lockStateInt);
    }

    if (source.hasField(process.name())) {
        std::string lockProcess;
        Status status = bsonExtractStringField(source, process.name(), &lockProcess);
        if (!status.isOK())
            return status;
        lock._process = lockProcess;
    }

    if (source.hasField(lockID.name())) {
        BSONElement lockIDElem;
        Status status = bsonExtractTypedField(source, lockID.name(), BSONType::jstOID, &lockIDElem);
        if (!status.isOK())
            return status;
        lock._lockID = lockIDElem.OID();
    }

    if (source.hasField(who.name())) {
        std::string lockWho;
        Status status = bsonExtractStringField(source, who.name(), &lockWho);
        if (!status.isOK())
            return status;
        lock._who = lockWho;
    }

    if (source.hasField(why.name())) {
        std::string lockWhy;
        Status status = bsonExtractStringField(source, why.name(), &lockWhy);
        if (!status.isOK())
            return status;
        lock._why = lockWhy;
    }

    return lock;
}

Status LocksType::validate() const {
    if (!_name.is_initialized() || _name->empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << name.name() << " field"};
    }

    if (!_state.is_initialized()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << state.name() << " field"};
    }

    State lockState = getState();
    if (lockState < 0 || lockState >= State::numStates) {
        return {ErrorCodes::BadValue, str::stream() << "Invalid lock state: " << getState()};
    }

    // if the lock is locked, check the remaining fields
    if (lockState != State::UNLOCKED) {
        if (!_process.is_initialized() || _process->empty()) {
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "missing " << process.name() << " field"};
        }

        if (!_lockID.is_initialized()) {
            return {ErrorCodes::NoSuchKey,
                    str::stream() << "missing " << lockID.name() << " field"};
        }

        if (!_who.is_initialized() || _who->empty()) {
            return {ErrorCodes::NoSuchKey, str::stream() << "missing " << who.name() << " field"};
        }

        if (!_why.is_initialized() || _why->empty()) {
            return {ErrorCodes::NoSuchKey, str::stream() << "missing " << why.name() << " field"};
        }
    }

    return Status::OK();
}

BSONObj LocksType::toBSON() const {
    BSONObjBuilder builder;

    if (_name)
        builder.append(name.name(), getName());
    if (_state)
        builder.append(state.name(), getState());
    if (_process)
        builder.append(process.name(), getProcess());
    if (_lockID)
        builder.append(lockID.name(), getLockID());
    if (_who)
        builder.append(who.name(), getWho());
    if (_why)
        builder.append(why.name(), getWhy());

    return builder.obj();
}

void LocksType::setName(const std::string& name) {
    invariant(!name.empty());
    _name = name;
}

void LocksType::setState(const State state) {
    invariant(state >= 0 && state < LocksType::numStates);
    _state = state;
}

void LocksType::setProcess(const std::string& process) {
    invariant(!process.empty());
    _process = process;
}

void LocksType::setLockID(const OID& lockID) {
    invariant(lockID.isSet());
    _lockID = lockID;
}

void LocksType::setWho(const std::string& who) {
    invariant(!who.empty());
    _who = who;
}

void LocksType::setWhy(const std::string& why) {
    invariant(!why.empty());
    _why = why;
}

std::string LocksType::toString() const {
    return toBSON().toString();
}

}  // namespace mongo
