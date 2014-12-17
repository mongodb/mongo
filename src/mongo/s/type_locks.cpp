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
#include "mongo/s/type_locks.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string LocksType::ConfigNS = "config.locks";

    const BSONField<std::string> LocksType::name("_id");
    const BSONField<int> LocksType::state("state");
    const BSONField<std::string> LocksType::process("process");
    const BSONField<OID> LocksType::lockID("ts");
    const BSONField<std::string> LocksType::who("who");
    const BSONField<std::string> LocksType::why("why");

    LocksType::LocksType() {
        clear();
    }

    LocksType::~LocksType() {
    }

    bool LocksType::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isNameSet) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }
        if (!_isStateSet) {
            *errMsg = stream() << "missing " << state.name() << " field";
            return false;
        }

        // If the lock is locked check the remaining fields
        if (_state != 0) {
            if (!_isProcessSet) {
                *errMsg = stream() << "missing " << process.name() << " field";
                return false;
            }

            if (!_isLockIDSet) {
                *errMsg = stream() << "missing " << lockID.name() << " field";
                return false;
            }

            if (!_isWhoSet) {
                *errMsg = stream() << "missing " << who.name() << " field";
                return false;
            }

            if (!_isWhySet) {
                *errMsg = stream() << "missing " << why.name() << " field";
                return false;
            }
        }

        return true;
    }

    BSONObj LocksType::toBSON() const {
        BSONObjBuilder builder;

        if (_isNameSet) builder.append(name(), _name);
        if (_isStateSet) builder.append(state(), _state);
        if (_isProcessSet) builder.append(process(), _process);
        if (_isLockIDSet) builder.append(lockID(), _lockID);
        if (_isWhoSet) builder.append(who(), _who);
        if (_isWhySet) builder.append(why(), _why);

        return builder.obj();
    }

    bool LocksType::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, name, &_name, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isNameSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, state, &_state, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isStateSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, process, &_process, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isProcessSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, lockID, &_lockID, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isLockIDSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, who, &_who, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isWhoSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, why, &_why, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isWhySet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void LocksType::clear() {

        _name.clear();
        _isNameSet = false;

        _state = 0;
        _isStateSet = false;

        _process.clear();
        _isProcessSet = false;

        _lockID = OID();
        _isLockIDSet = false;

        _who.clear();
        _isWhoSet = false;

        _why.clear();
        _isWhySet = false;

    }

    void LocksType::cloneTo(LocksType* other) const {
        other->clear();

        other->_name = _name;
        other->_isNameSet = _isNameSet;

        other->_state = _state;
        other->_isStateSet = _isStateSet;

        other->_process = _process;
        other->_isProcessSet = _isProcessSet;

        other->_lockID = _lockID;
        other->_isLockIDSet = _isLockIDSet;

        other->_who = _who;
        other->_isWhoSet = _isWhoSet;

        other->_why = _why;
        other->_isWhySet = _isWhySet;

    }

    std::string LocksType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
