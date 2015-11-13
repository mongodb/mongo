/**
 *    Copyright (C) 2015 MongoDB Inc.
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
     * Copy `inLen` bytes from `in`, process them, and write the processed bytes into `out`.
     * As processing may produce more or fewer bytes than were provided, the actual number
     * of bytes written will placed in `bytesWritten`.
     */
    virtual Status protect(const std::uint8_t* in,
                           std::size_t inLen,
                           std::uint8_t* out,
                           std::size_t outLen,
                           std::size_t* bytesWritten) = 0;

    /**
     * Declares that this DataProtector will be provided no more data to protect.
     * Fills `out` with any leftover state that needs serialization.
     */
    virtual Status finalize(std::uint8_t* out, std::size_t outLen, std::size_t* bytesWritten) = 0;

    /**
     * Returns the number of bytes reserved for metadata at the beginning of the first output
     * buffer.
     * Not all implementations will choose to reserve this space. They will return 0.
     */
    virtual std::size_t getNumberOfBytesReservedForTag() const = 0;

    /**
     * Fills buffer `out` of size `outLen`, with implementation defined metadata that had to be
     * calculated after finalization.
     * `bytesWritten` is filled with the number of bytes written into `out`.
     */
    virtual Status finalizeTag(std::uint8_t* out,
                               std::size_t outLen,
                               std::size_t* bytesWritten) = 0;
};

}  // namespace mongo
