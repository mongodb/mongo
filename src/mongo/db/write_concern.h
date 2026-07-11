// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
 * Optional hook to remap write concern options before validation. Modules that restrict supported
 * write concerns (e.g. DSC) may set this at startup to transparently remap
 * unsupported values (such as w:N for N>1) to an equivalent supported form.
 */
extern std::function<void(WriteConcernOptions&)> remapWriteConcernHook;

/**
 * Attempts to extract a WriteConcernOptions from a command's generic arguments.
 * Verifies that the resulting writeConcern is valid for this particular host.
 */
StatusWith<WriteConcernOptions> extractWriteConcern(OperationContext* opCtx,
                                                    const GenericArguments& invocation,
                                                    std::string_view commandName,
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


}  // namespace mongo
