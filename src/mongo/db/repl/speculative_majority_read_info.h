// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/util/modules.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
class [[MONGO_MOD_PUBLIC]] SpeculativeMajorityReadInfo {
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
