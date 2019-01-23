
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
#include "mongo/db/repl/optime.h"

namespace mongo {

class OperationContext;

namespace repl {

/**
 * This structure stores information about a "speculative" majority read. Reads of this nature do
 * not rely on storage engine support for reading from a historical, majority committed snapshot.
 * Instead, they read from a local (uncommitted) snapshot of data, and, at the end of command
 * execution, block to ensure that any results returned are majority committed.
 *
 * For the sake of correctness, it would be fine to always wait on the most recent lastApplied
 * optime after a command completes, but, as an optimization, we also allow commands to choose what
 * optime to wait on. For example, if a command generates a batch of data and it knows that the data
 * doesn't reflect any operations newer than optime T, it can set the "speculative majority optime"
 * to T. If T is much less than lastApplied, this can save unnecessary waiting.
 *
 * The basic theory of operation of this class is that once an operation has been marked as a
 * speculative majority read, it cannot be "unmarked" as such. Trying to mark it as speculative
 * again should have no effect. Once it has been marked as "speculative", a specific optime to wait
 * on can also be optionally provided. This read optime can be set multiple times, but every new
 * optime must be >= the previously set optime. This ensures that clients of this class utilize it
 * sensibly. The goal is to avoid a later caller invalidating the optime set by an earlier caller,
 * by setting the optime backwards. By enforcing the monotonicity of optimes, we ensure that waiting
 * on any read optime of this class will always guarantee that previously set optimes are also
 * majority committed. If no optime is ever set, then a speculative read operation should wait on
 * the most recent, system-wide lastApplied optime after completing.
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
     * Set a speculative majority optime for this operation to wait on.
     *
     * May only be called if this operation has already been marked as a speculative read.
     *
     * If no read optime has been set already, then this will set the speculative read optime to the
     * optime given. If a speculative read optime has already been set at T, then any subsequent
     * call to this method with an optime T' only updates the speculative read optime if T' > T.
     * This guarantees that speculative read optimes advance monotonically.
     */
    void setSpeculativeReadOpTimeForward(const OpTime& opTime);

    /**
     * Get the speculative read optime for this operation, if one exists.
     *
     * Only valid to call if this operation is a speculative read. Returns boost::none if there is
     * no provided optime to wait on. If this returns boost::none, indicates that this speculative
     * read operation should wait on lastApplied.
     */
    boost::optional<OpTime> getSpeculativeReadOpTime();

private:
    // Is this operation a speculative majority read.
    bool _isSpeculativeRead = false;

    // An optional optime to wait on to become majority committed, if we are doing a speculative
    // majority read. This can only be non-empty if this operation is a speculative read.
    boost::optional<OpTime> _speculativeReadOpTime;
};

}  // namespace repl
}  // namespace mongo
