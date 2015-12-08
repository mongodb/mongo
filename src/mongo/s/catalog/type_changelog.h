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
 * This class represents the layout and contents of documents contained in the config.changelog or
 * config.actionlog collections. All manipulation of documents coming from that collection should be
 * done with this class.
 */
class ChangeLogType {
public:
    // Field names and types in the changelog collection type.
    static const BSONField<std::string> changeId;
    static const BSONField<std::string> server;
    static const BSONField<std::string> clientAddr;
    static const BSONField<Date_t> time;
    static const BSONField<std::string> what;
    static const BSONField<std::string> ns;
    static const BSONField<BSONObj> details;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Constructs a new ChangeLogType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<ChangeLogType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise, returns NoSuchKey
     * and information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const std::string& getChangeId() const {
        return _changeId.get();
    }
    void setChangeId(const std::string& changeId);

    const std::string& getServer() const {
        return _server.get();
    }
    void setServer(const std::string& server);

    const std::string& getClientAddr() const {
        return _clientAddr.get();
    }
    void setClientAddr(const std::string& clientAddr);

    const Date_t& getTime() const {
        return _time.get();
    }
    void setTime(const Date_t& time);

    const std::string& getWhat() const {
        return _what.get();
    }
    void setWhat(const std::string& what);

    const std::string& getNS() const {
        return _ns.get();
    }
    void setNS(const std::string& ns);

    const BSONObj& getDetails() const {
        return _details.get();
    }
    void setDetails(const BSONObj& details);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M)  id for this change "<hostname>-<current_time>-<increment>"
    boost::optional<std::string> _changeId;
    // (M)  hostname of server that we are making the change on.  Does not include port.
    boost::optional<std::string> _server;
    // (M)  hostname:port of the client that made this change
    boost::optional<std::string> _clientAddr;
    // (M)  time this change was made
    boost::optional<Date_t> _time;
    // (M)  description of the change
    boost::optional<std::string> _what;
    // (O) database or collection this change applies to
    boost::optional<std::string> _ns;
    // (M)  A BSONObj containing extra information about some operations
    boost::optional<BSONObj> _details;
};

}  // namespace mongo
