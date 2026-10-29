// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * The result of checking a thing's readConcern support.  There are two parts:
 * - Whether or not the thing supports the given readConcern.
 * - Whether or not the thing permits having the default readConcern applied.
 *
 * The thing is a command invocation, an aggregation pipeline, or an aggregation stage.
 */
struct ReadConcernSupportResult {
    /**
     * Convenience method to explicitly return a ReadConcernSupportResult which both supports the
     * given read concern and permits the default cluster-wide read concern to be applied.
     */
    static ReadConcernSupportResult allSupportedAndDefaultPermitted() {
        return {Status::OK(), Status::OK(), Status::OK()};
    }

    /**
     * Whether this thing supports the requested readConcern level (and if not, why not).
     */
    Status readConcernSupport;

    /**
     * Whether this thing permits the default readConcern to be applied (and if not, why not).
     */
    Status defaultReadConcernPermit;

    /*
     * Whether this permits the implicit default readConcern to be applied (and if not, why not).
     */
    Status implicitDefaultReadConcernPermit;

    /**
     * Construct with the given Statuses, or default to Status::OK if omitted.
     */
    ReadConcernSupportResult(boost::optional<Status> readConcernStatus,
                             boost::optional<Status> defaultReadConcernStatus)
        : readConcernSupport(readConcernStatus.value_or(Status::OK())),
          defaultReadConcernPermit(defaultReadConcernStatus.value_or(Status::OK())),
          implicitDefaultReadConcernPermit(Status::OK()) {}

    /**
     * Construct with the given Statuses, or default to Status::OK if omitted.
     */
    ReadConcernSupportResult(boost::optional<Status> readConcernStatus,
                             boost::optional<Status> defaultReadConcernStatus,
                             boost::optional<Status> implicitDefaultReadConcernStatus)
        : readConcernSupport(readConcernStatus.value_or(Status::OK())),
          defaultReadConcernPermit(defaultReadConcernStatus.value_or(Status::OK())),
          implicitDefaultReadConcernPermit(
              implicitDefaultReadConcernStatus.value_or(Status::OK())) {}

    /**
     * Combine the contents of another ReadConcernSupportResult with this one. The outcome is that,
     * for each of supported and permitted, this ReadConcernSupportResult will be non-OK if either
     * it or the other one is non-OK. If both are non-OK then preference is given to this one's
     * Status.
     */
    void merge(const ReadConcernSupportResult& other) {
        if (readConcernSupport.isOK()) {
            readConcernSupport = other.readConcernSupport;
        }
        if (defaultReadConcernPermit.isOK()) {
            defaultReadConcernPermit = other.defaultReadConcernPermit;
        }
    }
};

}  // namespace mongo
