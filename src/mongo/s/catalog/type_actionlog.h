/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 * config.actionlog collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class ActionLogType {
public:
    // Name of the actionlog collection in the config server.
    static const std::string ConfigNS;

    // Field names and types in the actionlog collection type.
    static const BSONField<std::string> server;
    static const BSONField<std::string> what;
    static const BSONField<Date_t> time;
    static const BSONField<BSONObj> details;

    // Common field names included under details
    static const BSONField<int> candidateChunks;
    static const BSONField<int> chunksMoved;
    static const BSONField<bool> didError;
    static const BSONField<long long> executionTimeMicros;
    static const BSONField<std::string> errmsg;

    /**
     * Constructs a new ActionLogType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<ActionLogType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fiels have been set. Otherwise, returns NoSuchKey
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

    const std::string& getServer() const {
        return _server.get();
    }
    void setServer(const std::string& server);

    const std::string& getWhat() const {
        return _what.get();
    }
    void setWhat(const std::string& what);

    const Date_t& getTime() const {
        return _time.get();
    }
    void setTime(const Date_t& time);

    /*
     * Builds the details object for the actionlog.
     * Current formats for detail are:
     * Success: {
     *           "candidateChunks" : ,
     *           "chunksMoved" : ,
     *           "executionTimeMillis" : ,
     *           "errorOccured" : false
     *          }
     * Failure: {
     *           "executionTimeMillis" : ,
     *           "errmsg" : ,
     *           "errorOccured" : true
     *          }
     * @param errMsg: set if a balancer round resulted in an error
     * @param executionTime: the time this round took to run
     * @param candidateChunks: the number of chunks identified to be moved
     * @param chunksMoved: the number of chunks moved
     */
    void setDetails(const boost::optional<std::string>& errMsg,
                    int executionTime,
                    int candidateChunks,
                    int chunksMoved);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // (M) hostname of server that we are making the change on. Does not include the port.
    boost::optional<std::string> _server;
    // (M) what the action being performed is.
    boost::optional<std::string> _what;
    // (M) time this change was made.
    boost::optional<Date_t> _time;
    // (M) A BSON document containing extra information about some operations
    boost::optional<BSONObj> _details;
};

}  // namespace mongo
