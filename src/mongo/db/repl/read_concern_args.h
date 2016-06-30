/**
 *    Copyright (C) 2015 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;

namespace repl {

enum class ReadConcernLevel { kLocalReadConcern, kMajorityReadConcern, kLinearizableReadConcern };

class ReadConcernArgs {
public:
    static const std::string kReadConcernFieldName;
    static const std::string kAfterOpTimeFieldName;
    static const std::string kLevelFieldName;

    ReadConcernArgs();
    ReadConcernArgs(boost::optional<OpTime> opTime, boost::optional<ReadConcernLevel> level);

    /**
     * Format:
     * {
     *    find: “coll”,
     *    filter: <Query Object>,
     *    readConcern: { // optional
     *      level: "[majority|local|linearizable]",
     *      afterOpTime: { ts: <timestamp>, term: <NumberLong> },
     *    }
     * }
     */
    Status initialize(const BSONObj& cmdObj) {
        return initialize(cmdObj[kReadConcernFieldName]);
    }

    /**
     * Initializes the object from the readConcern element in a command object.
     * Use this if you are already iterating over the fields in the command object.
     * This method correctly handles missing BSONElements.
     */
    Status initialize(const BSONElement& readConcernElem);

    /**
     * Appends level and afterOpTime.
     */
    void appendInfo(BSONObjBuilder* builder) const;

    /**
     * Returns whether these arguments are 'empty' in the sense that no read concern has been
     * requested.
     */
    bool isEmpty() const;

    ReadConcernLevel getLevel() const;
    OpTime getOpTime() const;
    BSONObj toBSON() const;
    std::string toString() const;

private:
    boost::optional<OpTime> _opTime;
    boost::optional<ReadConcernLevel> _level;
};

}  // namespace repl
}  // namespace mongo
