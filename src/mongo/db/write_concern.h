/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/write_concern_options.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class OperationContext;
template <typename T>
class StatusWith;

namespace repl {
class OpTime;
}

/**
 * Attempts to extract a writeConcern from cmdObj.
 * Verifies that the writeConcern is of type Object (BSON type) and
 * that the resulting writeConcern is valid for this particular host.
 */
StatusWith<WriteConcernOptions> extractWriteConcern(OperationContext* txn,
                                                    const BSONObj& cmdObj,
                                                    const std::string& dbName,
                                                    const bool supportsWriteConcern);

/**
 * Verifies that a WriteConcern is valid for this particular host.
 */
Status validateWriteConcern(OperationContext* txn, const WriteConcernOptions& writeConcern);

struct WriteConcernResult {
    WriteConcernResult() {
        reset();
    }

    void reset() {
        syncMillis = -1;
        fsyncFiles = -1;
        wTimedOut = false;
        wTime = -1;
        err = "";
    }

    void appendTo(const WriteConcernOptions& writeConcern, BSONObjBuilder* result) const;

    int syncMillis;
    int fsyncFiles;

    bool wTimedOut;
    int wTime;
    std::vector<HostAndPort> writtenTo;

    std::string err;  // this is the old err field, should deprecate
};

/**
 * Blocks until the database is sure the specified user write concern has been fulfilled, or
 * returns an error status if the write concern fails.  Does no validation of the input write
 * concern, it is an error to pass this function an invalid write concern for the host.
 *
 * Takes a user write concern as well as the replication opTime the write concern applies to -
 * if this opTime.isNull() no replication-related write concern options will be enforced.
 *
 * Returns result of the write concern if successful.
 * Returns NotMaster if the host steps down while waiting for replication
 * Returns UnknownReplWriteConcern if the wMode specified was not enforceable
 */
Status waitForWriteConcern(OperationContext* txn,
                           const repl::OpTime& replOpTime,
                           const WriteConcernOptions& writeConcern,
                           WriteConcernResult* result);


}  // namespace mongo
