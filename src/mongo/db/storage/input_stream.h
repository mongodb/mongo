/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <fmt/format.h>
#include <utility>

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {

/**
 * This template class provides a standardized input facility over StreamableInput or SeekableInput.
 *
 * This class inherits publicly from 'InputT' and expose all methods except InputT::read() so that
 * only InputStream::readBytes() is exposed for reading data but all other methods are exposed. If
 * 'InputT' is seekable, seek() method is still exposed and the client can seek to a specific
 * position before reading data with buffering.
 *
 * Type requirement: 'InputT' must meet the StreamableInput or SeekableInput concept, e.g. the
 * NamedPipeInput class, which is the first class used as InputStream's parent.
 */
template <typename InputT>
class InputStream : public InputT {
public:
    template <typename... ArgT>
    InputStream(ArgT&&... args) : InputT(std::forward<ArgT>(args)...) {
        using namespace fmt::literals;
        InputT::open();
        uassert(ErrorCodes::FileNotOpen,
                "error"_format(getErrorMessage("open"_sd, InputT::getPath())),
                InputT::isOpen());
    }

    /**
     * Reads 'count' bytes from the stream into the caller-provided 'buffer'. Caller is responsible
     * for ensuring 'buffer' size is >= 'count' bytes. The only reasons it should return fewer bytes
     * is if it reaches EOF or gets an error. (Ensuring this requires looping on Windows, as a
     * synchronous read, which we are using, will still return as soon as any bytes are available.)
     *
     * If any bytes were read AND no error was hit, the number read is returned. Always returns -1
     * on error, even if some bytes were read successfully, so the caller will not see those bytes.
     * EOF is not treated as an error, so repeated attempts to read past EOF will just return 0.
     * The design avoids having to construct std::pair return values to return multiple pieces of
     * information, as that would be much more expensive than returning an int, and this method may
     * be called billions of times in a scan.
     *
     * May throw an exception when count < 0.
     */
    int readBytes(int count, char* buffer) {
        tassert(7005000, "Number of bytes to read must be greater than 0", count > 0);

        int nReadTotal = 0;
        int nReadNow = 0;
        do {
            if (MONGO_likely(InputT::isGood())) {
                nReadNow = InputT::read((buffer + nReadTotal), (count - nReadTotal));
                nReadTotal += nReadNow;
            } else {
                break;
            }
            if (MONGO_likely(nReadTotal == count)) {
                return nReadTotal;
            }
        } while (nReadTotal < count && nReadNow > 0);
        // If we reach this point, we accumulated fewer than 'count' bytes.

        if (MONGO_likely(InputT::isEof())) {
            LOGV2_INFO(7005001, "Named pipe is closed", "path"_attr = InputT::getPath());
            return nReadTotal;
        }

        tassert(7005002, "Expected an error condition but succeeded", InputT::isFailed());
        LOGV2_ERROR(7005003,
                    "Failed to read a named pipe",
                    "error"_attr = getErrorMessage("read", InputT::getPath()));

        return -1;
    }

private:
    // Hides InputT::read() so that readBytes() is the external interface for reading.
    using InputT::read;
};
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
