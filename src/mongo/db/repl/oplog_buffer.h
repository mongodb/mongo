/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <cstddef>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"

namespace mongo {
namespace repl {

/**
 * Interface for temporary container of oplog entries (in BSON format) from sync source by
 * OplogFetcher that will be read by applier in the DataReplicator.
 *
 * Implementations are only required to support one pusher and one popper.
 */
class OplogBuffer {
    MONGO_DISALLOW_COPYING(OplogBuffer);

public:
    /**
     * Type of item held in the oplog buffer;
     */
    using Value = BSONObj;

    /**
     * Batch of oplog entries (in BSON format) for bulk read/write operations.
     */
    using Batch = std::vector<Value>;

    OplogBuffer() = default;
    virtual ~OplogBuffer() = default;

    /**
     * Causes the oplog buffer to initialize its internal state (start threads if appropriate,
     * create backing storage, etc). This method may be called at most once for the lifetime of an
     * oplog buffer.
     */
    virtual void startup() = 0;

    /**
     * Signals to the oplog buffer that it should shut down. This method may block. After
     * calling shutdown, it is illegal to perform read/write operations on this oplog buffer.
     *
     * It is legal to call this method multiple times, but it should only be called after startup
     * has been called.
     */
    virtual void shutdown() = 0;

    /**
     * Pushes operations in the iterator range [begin, end) into the oplog buffer without blocking.
     *
     * Returns false if there is insufficient space to complete this operation successfully.
     */
    virtual bool pushAllNonBlocking(Batch::const_iterator begin, Batch::const_iterator end) = 0;

    /**
     * Returns when enough space is available.
     */
    virtual void waitForSpace(std::size_t size) = 0;

    /**
     * Total size of all oplog entries in this oplog buffer as measured by the BSONObj::size()
     * function.
     */
    virtual std::size_t getSize() const = 0;

    /**
     * Returns the number/count of items in the oplog buffer.
     */
    virtual std::size_t getCount() const = 0;

    /**
     * Clears oplog buffer.
     */
    virtual void clear() = 0;

    /**
     * Returns false if oplog buffer is empty. "value" is left unchanged.
     * Otherwise, removes last item (saves in "value") from the oplog buffer and returns true.
     */
    virtual bool tryPop(Value* value) = 0;

    /**
     * Returns false if oplog buffer is empty.
     * Otherwise, returns true and sets "value" to last item in oplog buffer.
     */
    virtual bool peek(Value* value) = 0;
};

}  // namespace repl
}  // namespace mongo
