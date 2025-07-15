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

#include "mongo/base/data_range.h"

#include <cstddef>
#include <cstdint>

namespace mongo {

class Status;

/**
 * Performs an implementation specific transformation on a series of input buffers to
 * produce a protected form of their concatenated contents.
 *
 * protect() must be called on each input buffer, to produce a series of output buffers.
 * The caller must ensure that the output buffers are large enough to contain the protected
 * data resulting from the call. The caller must concatenate the output buffers together, in order.
 * Once all input buffers have been protect()ed, finalize() must be called, and its output
 * appended to the end of the protected data.
 * The caller may then call finalizeTag() to get implementation defined metadata.
 */
class DataProtector {
public:
    virtual ~DataProtector() = default;

    /**
     * Copies all bytes in `in`, processes them, and writes the processed bytes into `out`.
     * As processing may produce more or fewer bytes than were provided, `out` will point to
     * a DataRange with the actual length of produced bytes.
     */
    virtual Status protect(ConstDataRange in, DataRange* out) = 0;

    /**
     * Declares that this DataProtector will be provided no more data to protect.
     * Fills `out` with any leftover state that needs serialization.
     */
    virtual Status finalize(DataRange* out) = 0;

    /**
     * Returns the number of bytes reserved for metadata at the beginning of the first output
     * buffer.
     * Not all implementations will choose to reserve this space. They will return 0.
     */
    virtual std::size_t getNumberOfBytesReservedForTag() const = 0;

    /**
     * Fills buffer `out` with implementation defined metadata that had to be
     * calculated after finalization.
     * After successfully writing tag, `out` will be a DataRange with the length of data written.
     */
    virtual Status finalizeTag(DataRange* out) = 0;
};

}  // namespace mongo
