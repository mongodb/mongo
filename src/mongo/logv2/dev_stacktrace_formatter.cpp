// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/dev_stacktrace_formatter.h"

#include "mongo/logv2/attributes.h"

#include <boost/log/attributes/value_extraction.hpp>

namespace mongo::logv2 {

void DevStacktraceFormatter::operator()(boost::log::record_view const& rec,
                                        boost::log::formatting_ostream& strm) const {
    auto devStacktraceOpt = extract_or_default<bool>(attributes::devStacktrace(), rec, false);
    if (MONGO_unlikely(devStacktraceOpt)) {
        _plainFormatter(rec, strm);
    } else {
        _jsonFormatter(rec, strm);
    }
}

}  // namespace mongo::logv2
