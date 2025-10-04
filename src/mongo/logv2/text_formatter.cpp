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

#include "mongo/logv2/text_formatter.h"

#include "mongo/base/string_data.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/util/time_support.h"

#include <boost/exception/exception.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>
#include <fmt/format.h>

namespace mongo::logv2 {

void TextFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    using boost::log::extract;

    fmt::memory_buffer buffer;
    fmt::format_to(std::back_inserter(buffer),
                   "{} {:<2} {:<8} [{}] ",
                   StringData{DateStringBuffer{}.iso8601(
                       extract<Date_t>(attributes::timeStamp(), rec).get(),
                       _timestampFormat == LogTimestampFormat::kISO8601Local)},
                   extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact(),
                   extract<LogComponent>(attributes::component(), rec).get().getNameForLog(),
                   extract<StringData>(attributes::threadName(), rec).get());
    strm.write(buffer.data(), buffer.size());

    if (extract<LogTag>(attributes::tags(), rec).get().has(LogTag::kStartupWarnings)) {
        strm << "** WARNING: ";
    }

    PlainFormatter::operator()(rec, strm);
}

}  // namespace mongo::logv2
