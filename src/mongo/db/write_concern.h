/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"

#include <string>
#include <vector>

namespace MONGO_MOD_PUB mongo {

class OperationContext;
template <typename T>
class StatusWith;

namespace repl {
class OpTime;
}

/**
 * Returns true if 'cmdObj' has a 'writeConcern' field.
 */
bool commandSpecifiesWriteConcern(const GenericArguments& requestArgs);

/**
 * Attempts to extract a WriteConcernOptions from a command's generic arguments.
 * Verifies that the resulting writeConcern is valid for this particular host.
 */
StatusWith<WriteConcernOptions> extractWriteConcern(OperationContext* opCtx,
                                                    const GenericArguments& invocation,
                                                    StringData commandName,
                                                    bool isInternalClient);

/**
 * Verifies that a WriteConcern is valid for this particular host.
 */
Status validateWriteConcern(OperationContext* opCtx, const WriteConcernOptions& writeConcern);

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
        wcUsed = WriteConcernOptions();
    }

    void appendTo(BSONObjBuilder* result) const;

    int syncMillis;
    bool wTimedOut;
    int wTime;
    std::vector<HostAndPort> writtenTo;
    WriteConcernOptions wcUsed;

    std::string err;  // this is the old err field, should deprecate

    // This field has had a dummy value since MMAP went away. It is undocumented.
    // Maintaining it so as not to cause unnecessary user pain across upgrades.
    int fsyncFiles;
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
 * Returns NotWritablePrimary if the host steps down while waiting for replication
 * Returns UnknownReplWriteConcern if the wMode specified was not enforceable
 */
Status waitForWriteConcern(OperationContext* opCtx,
                           const repl::OpTime& replOpTime,
                           const WriteConcernOptions& writeConcern,
                           WriteConcernResult* result);

/**
 * Used to simulated WriteConcernTimeouts in tests. If the failWaitForWriteConcernIfTimeoutSet
 * failpoint is enabled and the write concern has a timeout, it would return a WriteConcernTimeout
 * error
 */
boost::optional<repl::ReplicationCoordinator::StatusAndDuration>
_tryGetWCFailureFromFailPoint_ForTest(const repl::OpTime& replOpTime,
                                      const WriteConcernOptions& writeConcern);


}  // namespace MONGO_MOD_PUB mongo
