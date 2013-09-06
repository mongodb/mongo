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
     *     BSONObj query = QUERY(LocksType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(LocksType::ConfigNS, query);
     *
     *     // Process the response.
     *     LocksType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class LocksType {
        MONGO_DISALLOW_COPYING(LocksType);
    public:

        //
        // schema declarations
        //

        // Name of the locks collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the locks collection type.
        static const BSONField<std::string> name;
        static const BSONField<int> state;
        static const BSONField<std::string> process;
        static const BSONField<OID> lockID;
        static const BSONField<std::string> who;
        static const BSONField<std::string> why;

        //
        // locks type methods
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
         * latter contains valid values. Otherwise sets errMsg and returns false.
         */
        bool parseBSON(const BSONObj& source, std::string* errMsg);

        /**
         * Clears the internal state.
         */
        void clear();

        /**
         * Copies all the fields present in 'this' to 'other'.
         */
        void cloneTo(LocksType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setName(const StringData& name) {
            _name = name.toString();
            _isNameSet = true;
        }

        void unsetName() { _isNameSet = false; }

        bool isNameSet() const { return _isNameSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string getName() const {
            dassert(_isNameSet);
            return _name;
        }

        void setState(const int state) {
            _state = state;
            _isStateSet = true;
        }

        void unsetState() { _isStateSet = false; }

        bool isStateSet() const { return _isStateSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        int getState() const {
            dassert(_isStateSet);
            return _state;
        }

        // Optional Fields
        void setProcess(StringData& process) {
            _process = process.toString();
            _isProcessSet = true;
        }

        void unsetProcess() { _isProcessSet = false; }

        bool isProcessSet() const {
            return _isProcessSet || process.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        std::string getProcess() const {
            if (_isProcessSet) {
                return _process;
            } else {
                dassert(process.hasDefault());
                return process.getDefault();
            }
        }
        void setLockID(OID lockID) {
            _lockID = lockID;
            _isLockIDSet = true;
        }

        void unsetLockID() { _isLockIDSet = false; }

        bool isLockIDSet() const {
            return _isLockIDSet || lockID.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        OID getLockID() const {
            if (_isLockIDSet) {
                return _lockID;
            } else {
                dassert(lockID.hasDefault());
                return lockID.getDefault();
            }
        }
        void setWho(StringData& who) {
            _who = who.toString();
            _isWhoSet = true;
        }

        void unsetWho() { _isWhoSet = false; }

        bool isWhoSet() const {
            return _isWhoSet || who.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        std::string getWho() const {
            if (_isWhoSet) {
                return _who;
            } else {
                dassert(who.hasDefault());
                return who.getDefault();
            }
        }
        void setWhy(StringData& why) {
            _why = why.toString();
            _isWhySet = true;
        }

        void unsetWhy() { _isWhySet = false; }

        bool isWhySet() const {
            return _isWhySet || why.hasDefault();
        }

        // Calling get*() methods when the member is not set and has no default results in undefined
        // behavior
        std::string getWhy() const {
            if (_isWhySet) {
                return _why;
            } else {
                dassert(why.hasDefault());
                return why.getDefault();
            }
        }

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _name;     // (M)  name of the lock
        bool _isNameSet;
        int _state;     // (M)  0: Unlocked | 1: Locks in contention | 2: Lock held
        bool _isStateSet;
        std::string _process;     // (O)  optional if unlocked.  contains the (unique) identifier
        bool _isProcessSet;
        OID _lockID;     // (O)  optional if unlocked.  a unique identifier for the instance
        bool _isLockIDSet;
        std::string _who;     // (O)  optional if unlocked.  a note about why the lock is held,
        bool _isWhoSet;
        std::string _why;     // (O)  optional if unlocked.  a human readable description of the
        bool _isWhySet;
    };

} // namespace mongo
