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

#include "mongo/s/type_locks.h"

#include "mongo/s/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const string LocksType::ConfigNS = "config.locks";
    BSONField<string> LocksType::name("_id");
    BSONField<int> LocksType::state("state");
    BSONField<string> LocksType::process("process");
    BSONField<OID> LocksType::lockID("ts");
    BSONField<string> LocksType::who("who");
    BSONField<string> LocksType::why("why");

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
        if (_name.empty()) {
            *errMsg = stream() << "missing " << name.name() << " field";
            return false;
        }

        // If the lock is locked check the remaining fields
        if (_state != 0) {
            if (_process.empty()) {
                *errMsg = stream() << "missing " << process.name() << " field";
                return false;
            }

            if (! _lockID.isSet()) {
                *errMsg = stream() << "missing " << lockID.name() << " field";
                return false;
            }

            if (_who.empty()) {
                *errMsg = stream() << "missing " << who.name() << " field";
                return false;
            }

            if (_why.empty()) {
                *errMsg = stream() << "missing " << why.name() << " field";
                return false;
            }
        }
        return true;
    }

    BSONObj LocksType::toBSON() const {
        BSONObjBuilder builder;
        if (!_name.empty()) builder.append(name(), _name);
        builder.append(state(), _state);
        if (!_process.empty()) builder.append(process(), _process);
        if (!_who.empty()) builder.append(who(), _who);
        if (_lockID.isSet()) builder.append(lockID(), _lockID);
        if (!_why.empty()) builder.append(why(), _why);
        return builder.obj();
    }

    bool LocksType::parseBSON(BSONObj source, string* errMsg) {
        clear();

        string dummy;
        if (!errMsg) errMsg = &dummy;

        if (!FieldParser::extract(source, name, "", &_name, errMsg)) return false;
        if (!FieldParser::extract(source, state, 0, &_state, errMsg)) return false;
        if (!FieldParser::extract(source, process, "", &_process, errMsg)) return false;
        if (!FieldParser::extract(source, lockID, OID(), &_lockID, errMsg)) return false;
        if (!FieldParser::extract(source, who, "", &_who, errMsg)) return false;
        if (!FieldParser::extract(source, why, "", &_why, errMsg)) return false;

        return true;
    }

    void LocksType::clear() {
        _name.clear();
        _state = 0;
        _process.clear();
        _lockID = OID();
        _who.clear();
        _why.clear();
    }

    void LocksType::cloneTo(LocksType* other) {
        other->clear();
        other->_name = _name;
        other->_state = _state;
        other->_process = _process;
        other->_lockID = _lockID;
        other->_who = _who;
        other->_why = _why;
    }

    std::string LocksType::toString() const {
        return toBSON().toString();
    }

} // namespace mongo
