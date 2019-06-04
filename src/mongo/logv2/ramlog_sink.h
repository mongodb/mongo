/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/unlocked_frontend.hpp>
#include <boost/make_shared.hpp>
#include <string>

#include "mongo/logv2/ramlog.h"

namespace mongo {
namespace logv2 {

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

    static boost::shared_ptr<boost::log::sinks::unlocked_sink<RamLogSink>> create(RamLog* ramlog) {
        using namespace boost::log;

        auto backend = boost::make_shared<RamLogSink>(ramlog);

        auto sink =
            boost::make_shared<boost::log::sinks::unlocked_sink<RamLogSink>>(boost::move(backend));

        return sink;
    }

    explicit RamLogSink(RamLog* ramlog) : _ramlog(ramlog) {}
    // The function consumes the log records that come from the frontend
    void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
        _ramlog->write(formatted_string);
    }

private:
    RamLog* _ramlog;
};

}  // namespace logv2
}  // namespace mongo
