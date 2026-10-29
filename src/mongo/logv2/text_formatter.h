// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/log_format.h"
#include "mongo/logv2/plain_formatter.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>

namespace mongo::logv2 {

class TextFormatter : protected PlainFormatter {
public:
    TextFormatter(const Atomic<int32_t>* maxAttributeSizeKB = nullptr,
                  LogTimestampFormat timestampFormat = LogTimestampFormat::kISO8601UTC)
        : PlainFormatter(maxAttributeSizeKB), _timestampFormat(timestampFormat) {}

    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) const;

private:
    const LogTimestampFormat _timestampFormat;
};

}  // namespace mongo::logv2
