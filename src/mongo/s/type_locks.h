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

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * This class represents the layout and contents of documents contained in the
     * config.locks collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(LocksType::name("balancer"));
     *     lockDoc = conn->findOne(LocksType::ConfigNS, query);
     *
     *     // Process the response.
     *     LocksType lock;
     *     lock.fromBSON(lockDoc);
     *     if (! lock.isValid()) {
     *         // Can't use 'lock'. Take action.
     *     }
     *     // use 'lock'
     *
     */
    class LocksType {
        MONGO_DISALLOW_COPYING(LocksType);
    public:

        //
        // schema declarations
        //

        // Name of the collection in the config server.
        static const std::string ConfigNS;

        static BSONField<std::string> name;       // name of the lock
        static BSONField<int> state;              // 0: Unlocked
                                                  // 1: Locks in contention
                                                  // 2: Lock held
        static BSONField<std::string> process;    // the process field contains the (unique)
                                                  // identifier for the instance of mongod/mongos
                                                  // which has requested the lock
        static BSONField<OID> lockID;             // a unique identifier for the instance of the
                                                  // lock itself. Allows for safe cleanup after
                                                  // network partitioning
        static BSONField<std::string> who;        // a note about why the lock is held, or which
                                                  // subcomponent is holding it
        static BSONField<std::string> why;        // a human readable description of the purpose of
                                                  // the lock

        //
        // collection type methods
        //

        LocksType();
        ~LocksType();

        /**
         * Returns true if all the mandatory fields are present and have valid
         * representations. Otherwise returns false and fills in the optional 'errMsg' string.
         */
        bool isValid(std::string* errMsg) const;

        /**
         * Returns the BSON representation of the entry.
         */
        BSONObj toBSON() const;

        /**
         * Clears and populates the internal state using the 'source' BSON object if the
         * latter contains valid values. Otherwise clear the internal state.
         */
        void parseBSON(BSONObj source);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(LocksType* other);

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        void setName(const StringData& name) { _name = name.toString(); }
        const std::string& getName() const { return _name; }

        void setState(int state) { _state = state; }
        int getState() const { return _state; }

        void setProcess(const StringData& process) {
            _process = process.toString();
        }
        const std::string& getProcess() const { return _process; }

        void setLockID(OID lockID) { _lockID = lockID; }
        OID getLockID() const { return _lockID; }

        void setWho(const StringData& who) { _who = who.toString(); }
        const std::string& getWho() const { return _who; }

        void setWhy(const StringData& why) { _why = why.toString(); }
        const std::string& getWhy() const { return _why; }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _name;    // (M) name of the lock
        int _state;           // (M) 0: Unlocked | 1: Locks in contention | 2: Lock held
        std::string _process; // (S) optional if unlocked.  contains the (unique) identifier
                              // for the instance of mongod/mongos which has requested the lock
        OID _lockID;          // (S) optional if unlocked.  a unique identifier for the instance
                              // of the lock itself. Allows for safe cleanup after network
                              // partitioning
        std::string _who;     // (S) optional if unlocked.  a note about why the lock is held,
                              // or which subcomponent is holding it
        std::string _why;     // (S) optional if unlocked.  a human readable description of the
                              // purpose of the lock
    };

} // namespace mongo
