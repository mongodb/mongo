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

#include "mongo/base/shim.h"

namespace mongo {

class BSONObj;
class OperationContext;
class Status;
template <typename T>
class StatusWith;
namespace repl {
class ReadConcernArgs;
class SpeculativeMajorityReadInfo;
}

/**
 * Given the specified read concern arguments, performs checks that the read concern can actually be
 * satisfied given the current state of the server and if so calls into the replication subsystem to
 * perform the wait. If allowAfterClusterTime is false returns an error if afterClusterTime is
 * set on the readConcernArgs. Both cmdName and readConcernArgs are used to determine whether or not
 * prepare conflicts can be ignored.
 */
extern MONGO_DECLARE_SHIM((OperationContext * opCtx,
                           const repl::ReadConcernArgs& readConcernArgs,
                           bool allowAfterClusterTime,
                           StringData cmdName)
                              ->Status) waitForReadConcern;

/*
 * Given a linearizable read command, confirm that
 * current primary is still the true primary of the replica set.
 *
 * A readConcernTimeout of 0 indicates that the operation will block indefinitely waiting for read
 * concern.
 */
extern MONGO_DECLARE_SHIM((OperationContext * opCtx, const int readConcernTimeout)->Status)
    waitForLinearizableReadConcern;

/**
 * Waits to satisfy a "speculative" majority read.
 *
 * This method must only be called if the operation is a speculative majority read.
 */
extern MONGO_DECLARE_SHIM((OperationContext * opCtx,
                           repl::SpeculativeMajorityReadInfo speculativeReadInfo)
                              ->Status) waitForSpeculativeMajorityReadConcern;

}  // namespace mongo
