// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/data_range.h"
#include "mongo/util/modules.h"

#include <cstddef>

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
class [[MONGO_MOD_OPEN]] DataProtector {
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
