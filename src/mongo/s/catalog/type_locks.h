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

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * This class represents the layout and contents of documents contained in the
 * config.locks collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class LocksType {
public:
    enum State {
        UNLOCKED = 0,
        LOCK_PREP,  // Only for legacy 3 config servers.
        LOCKED,
        numStates
    };

    // Name of the locks collection in the config server.
    static const std::string ConfigNS;

    // Field names and types in the locks collection type.
    static const BSONField<std::string> name;
    static const BSONField<State> state;
    static const BSONField<std::string> process;
    static const BSONField<OID> lockID;
    static const BSONField<std::string> who;
    static const BSONField<std::string> why;
    static const BSONField<Date_t> when;

    /**
     * Constructs a new LocksType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<LocksType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise, returns NoSuchKey
     * and information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const std::string& getName() const {
        return _name.get();
    }
    void setName(const std::string& name);

    State getState() const {
        return _state.get();
    }
    void setState(const State state);

    const std::string& getProcess() const {
        return _process.get();
    }
    void setProcess(const std::string& process);

    const OID& getLockID() const {
        return _lockID.get();
    }
    bool isLockIDSet() const {
        return _lockID.is_initialized() && _lockID->isSet();
    }
    void setLockID(const OID& lockID);

    const std::string& getWho() const {
        return _who.get();
    }
    void setWho(const std::string& who);

    const std::string& getWhy() const {
        return _why.get();
    }
    void setWhy(const std::string& why);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) name of the lock
    boost::optional<std::string> _name;
    // (M) State of the lock (see LocksType::State)
    boost::optional<State> _state;
    // (O) optional if unlocked. Contains the (unique) identifier.
    boost::optional<std::string> _process;
    // (O) optional if unlocked. A unique identifier for the instance.
    boost::optional<OID> _lockID;
    // (O) optional if unlocked. A note about why the lock is held.
    boost::optional<std::string> _who;
    // (O) optional if unlocked. A human readable description of why the lock is held.
    boost::optional<std::string> _why;
};

}  // namespace mongo
