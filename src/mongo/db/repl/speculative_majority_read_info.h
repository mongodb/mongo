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

#include <boost/optional.hpp>

#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"

namespace mongo {

class OperationContext;

namespace repl {

/**
 * This structure stores information about a "speculative" majority read. Reads of this nature do
 * not rely on storage engine support for reading from a historical, majority committed snapshot.
 * Instead, they read from a local (uncommitted) snapshot of data, and, at the end of command
 * execution, block to ensure that any results returned are majority committed.
 *
 * We allow commands to optionally choose what timestamp to wait on. For example, if a command
 * generates a batch of data and it knows that the data doesn't reflect any operations newer than
 * timestamp T, it can set the "speculative majority timestamp" to T. This is an optimization that
 * can save unnecessary waiting.
 *
 * The basic theory of operation of this class is that once an operation has been marked as a
 * speculative majority read, it cannot be "unmarked" as such. Trying to mark it as speculative
 * again should have no effect. Once it has been marked as "speculative", a specific timestamp to
 * wait on can also be optionally provided. This read timestamp can be set multiple times, but every
 * new timestamp must be >= the previously set timestamp. This ensures that clients of this class
 * utilize it sensibly. The goal is to avoid a later caller invalidating the timestamp set by an
 * earlier caller, by setting the timestamp backwards. By enforcing the monotonicity of timestamps,
 * we ensure that waiting on any read timestamp of this class will always guarantee that previously
 * set timestamps are also majority committed. If no timestamp is ever set, then it is up to the
 * user of this structure to determine what timestamp to wait on so that data read is guaranteed to
 * be majority committed. This, for example, could be the timestamp of the read source chosen by the
 * storage engine.
 */
class SpeculativeMajorityReadInfo {
public:
    static SpeculativeMajorityReadInfo& get(OperationContext* opCtx);

    /**
     * Mark an operation as a speculative majority read.
     *
     * If this operation has already been marked as a speculative read, this method does nothing.
     */
    void setIsSpeculativeRead();

    /**
     * Returns whether this operation is a speculative majority read.
     */
    bool isSpeculativeRead() const;

    /**
     * Set a speculative majority timestamp for this operation to wait on.
     *
     * May only be called if this operation has already been marked as a speculative read.
     *
     * If no read timestamp has been set already, then this will set the speculative read timestamp
     * to the timestamp given. If a speculative read timestamp has already been set at T, then any
     * subsequent call to this method with a timestamp T' only updates the speculative read
     * timestamp if T' > T. This guarantees that speculative read timestamps advance monotonically.
     */
    void setSpeculativeReadTimestampForward(const Timestamp& ts);

    /**
     * Get the speculative read timestamp for this operation, if one exists.
     *
     * Only valid to call if this operation is a speculative read. Returns boost::none if there is
     * no provided timestamp to wait on.
     */
    boost::optional<Timestamp> getSpeculativeReadTimestamp();

private:
    // Is this operation a speculative majority read.
    bool _isSpeculativeRead = false;

    // An optional timestamp to wait on to become majority committed, if we are doing a speculative
    // majority read. This can only be non-empty if this operation is a speculative read.
    boost::optional<Timestamp> _speculativeReadTimestamp;
};

}  // namespace repl
}  // namespace mongo
