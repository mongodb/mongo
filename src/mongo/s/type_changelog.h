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
     * config.changelog collection. All manipulation of documents coming from that
     * collection should be done with this class.
     *
     * Usage Example:
     *
     *     // Contact the config. 'conn' has been obtained before.
     *     DBClientBase* conn;
     *     BSONObj query = QUERY(ChangelogType::exampleField("exampleFieldName"));
     *     exampleDoc = conn->findOne(ChangelogType::ConfigNS, query);
     *
     *     // Process the response.
     *     ChangelogType exampleType;
     *     string errMsg;
     *     if (!exampleType.parseBSON(exampleDoc, &errMsg) || !exampleType.isValid(&errMsg)) {
     *         // Can't use 'exampleType'. Take action.
     *     }
     *     // use 'exampleType'
     *
     */
    class ChangelogType {
        MONGO_DISALLOW_COPYING(ChangelogType);
    public:

        //
        // schema declarations
        //

        // Name of the changelog collection in the config server.
        static const std::string ConfigNS;

        // Field names and types in the changelog collection type.
        static const BSONField<std::string> changeID;
        static const BSONField<std::string> server;
        static const BSONField<std::string> clientAddr;
        static const BSONField<Date_t> time;
        static const BSONField<std::string> what;
        static const BSONField<std::string> ns;
        static const BSONField<BSONObj> details;

        //
        // changelog type methods
        //

        ChangelogType();
        ~ChangelogType();

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
        void cloneTo(ChangelogType* other) const;

        /**
         * Returns a string representation of the current internal state.
         */
        std::string toString() const;

        //
        // individual field accessors
        //

        // Mandatory Fields
        void setChangeID(const StringData& changeID) {
            _changeID = changeID.toString();
            _isChangeIDSet = true;
        }

        void unsetChangeID() { _isChangeIDSet = false; }

        bool isChangeIDSet() const { return _isChangeIDSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getChangeID() const {
            dassert(_isChangeIDSet);
            return _changeID;
        }

        void setServer(const StringData& server) {
            _server = server.toString();
            _isServerSet = true;
        }

        void unsetServer() { _isServerSet = false; }

        bool isServerSet() const { return _isServerSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getServer() const {
            dassert(_isServerSet);
            return _server;
        }

        void setClientAddr(const StringData& clientAddr) {
            _clientAddr = clientAddr.toString();
            _isClientAddrSet = true;
        }

        void unsetClientAddr() { _isClientAddrSet = false; }

        bool isClientAddrSet() const { return _isClientAddrSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getClientAddr() const {
            dassert(_isClientAddrSet);
            return _clientAddr;
        }

        void setTime(const Date_t time) {
            _time = time;
            _isTimeSet = true;
        }

        void unsetTime() { _isTimeSet = false; }

        bool isTimeSet() const { return _isTimeSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const Date_t getTime() const {
            dassert(_isTimeSet);
            return _time;
        }

        void setWhat(const StringData& what) {
            _what = what.toString();
            _isWhatSet = true;
        }

        void unsetWhat() { _isWhatSet = false; }

        bool isWhatSet() const { return _isWhatSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getWhat() const {
            dassert(_isWhatSet);
            return _what;
        }

        void setNS(const StringData& ns) {
            _ns = ns.toString();
            _isNsSet = true;
        }

        void unsetNS() { _isNsSet = false; }

        bool isNSSet() const { return _isNsSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const std::string& getNS() const {
            dassert(_isNsSet);
            return _ns;
        }

        void setDetails(const BSONObj& details) {
            _details = details.getOwned();
            _isDetailsSet = true;
        }

        void unsetDetails() { _isDetailsSet = false; }

        bool isDetailsSet() const { return _isDetailsSet; }

        // Calling get*() methods when the member is not set results in undefined behavior
        const BSONObj getDetails() const {
            dassert(_isDetailsSet);
            return _details;
        }

        // Optional Fields

    private:
        // Convention: (M)andatory, (O)ptional, (S)pecial rule.
        std::string _changeID;     // (M)  id for this change "<hostname>-<current_time>-<increment>"
        bool _isChangeIDSet;
        std::string _server;     // (M)  hostname of server that we are making the change on.  Does not include port.
        bool _isServerSet;
        std::string _clientAddr;     // (M)  hostname:port of the client that made this change
        bool _isClientAddrSet;
        Date_t _time;     // (M)  time this change was made
        bool _isTimeSet;
        std::string _what;     // (M)  description of the change
        bool _isWhatSet;
        std::string _ns;     // (M)  database or collection this change applies to
        bool _isNsSet;
        BSONObj _details;     // (M)  A BSONObj containing extra information about some operations
        bool _isDetailsSet;
    };

} // namespace mongo
