// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/logv2/constants.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log_format.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>

namespace mongo::logv2 {

/**
 * This is used for dev stacktraces which bypass structured logging. LOGV2 sinks
 * are by default configured to use the JSONFormatter. We need to be able to choose
 * PlainFormatter for our dev stacktraces, and JSONFormatter for everything else.
 *
 * This class is only expected to be used when the bazel flag `dev_stacktrace` is
 * enabled.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DevStacktraceFormatter {
public:
    DevStacktraceFormatter(const Atomic<int32_t>* maxAttributeSizeKB,
                           LogTimestampFormat timestampFormat)
        : _plainFormatter(PlainFormatter(maxAttributeSizeKB)),
          _jsonFormatter(JSONFormatter(maxAttributeSizeKB, timestampFormat)) {}

    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) const;

private:
    PlainFormatter _plainFormatter;
    JSONFormatter _jsonFormatter;
};

}  // namespace mongo::logv2
