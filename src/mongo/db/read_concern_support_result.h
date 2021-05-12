/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

namespace mongo {

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
