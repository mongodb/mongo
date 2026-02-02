#pragma once

#include <istream>

namespace immer::util {

/*!
 * Restores the iostream state.
 *
 * This is particularly handy for utilities that can be used to peek or query
 * properties of a document read from a stream, while leaving the stream in the
 * original state.
 */
struct istream_snapshot
{
    std::reference_wrapper<std::istream> stream;
    std::istream::pos_type pos       = stream.get().tellg();
    std::istream::iostate state      = stream.get().rdstate();
    std::istream::iostate exceptions = stream.get().exceptions();

    istream_snapshot(std::istream& is)
        : stream{is}
    {
    }

    // It is not copyable nor movable
    istream_snapshot(istream_snapshot&&) = delete;

    ~istream_snapshot()
    {
        stream.get().exceptions(exceptions);
        stream.get().clear(state);
        stream.get().seekg(pos);
    }
};

} // namespace immer::util
