// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/ramlog.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/log/sinks/basic_sink_backend.hpp>

namespace mongo::logv2 {

class RamLogSink : public boost::log::sinks::
                       basic_formatted_sink_backend<char, boost::log::sinks::concurrent_feeding> {
private:
    //! Base type
    typedef boost::log::sinks::basic_formatted_sink_backend<char,
                                                            boost::log::sinks::concurrent_feeding>
        base_type;

public:
    //! Character type
    typedef typename base_type::char_type char_type;
    //! String type to be used as a message text holder
    typedef typename base_type::string_type string_type;
    //! Output stream type
    typedef std::basic_ostream<char_type> stream_type;

    explicit RamLogSink(RamLog* ramlog) : _ramlog(ramlog) {}
    // The function consumes the log records that come from the frontend
    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        _ramlog->write(formatted_string);
    }

private:
    RamLog* _ramlog;
};

}  // namespace mongo::logv2
