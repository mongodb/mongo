// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/text_formatter.h"

#include "mongo/logv2/attributes.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/util/time_support.h"

#include <string_view>

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
                   std::string_view{DateStringBuffer{}.iso8601(
                       extract<Date_t>(attributes::timeStamp(), rec).get(),
                       _timestampFormat == LogTimestampFormat::kISO8601Local)},
                   extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact(),
                   extract<LogComponent>(attributes::component(), rec).get().getNameForLog(),
                   extract<std::string_view>(attributes::threadName(), rec).get());
    strm.write(buffer.data(), buffer.size());

    if (extract<LogTag>(attributes::tags(), rec).get().has(LogTag::kStartupWarnings)) {
        strm << "** WARNING: ";
    }

    PlainFormatter::operator()(rec, strm);
}

}  // namespace mongo::logv2
