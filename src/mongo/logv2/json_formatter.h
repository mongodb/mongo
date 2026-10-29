// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_format.h"
#include "mongo/logv2/log_service.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_truncation.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <string>
#include <string_view>

#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>
#include <fmt/format.h>

namespace mongo::logv2 {

class [[MONGO_MOD_OPEN]] JSONFormatter {
public:
    JSONFormatter(const Atomic<int32_t>* maxAttributeSizeKB = nullptr,
                  LogTimestampFormat timestampFormat = LogTimestampFormat::kISO8601UTC)
        : _maxAttributeSizeKB(maxAttributeSizeKB), _timestampFormat(timestampFormat) {}

    void format(fmt::memory_buffer& buffer,
                LogSeverity severity,
                LogComponent component,
                Date_t date,
                int32_t id,
                LogService service,
                std::string_view context,
                std::string_view message,
                const TypeErasedAttributeStorage& attrs,
                LogTag tags,
                const std::string& tenant,
                LogTruncation truncation) const;
    void operator()(boost::log::record_view const& rec, boost::log::formatting_ostream& strm) const;

private:
    const Atomic<int32_t>* _maxAttributeSizeKB;
    const LogTimestampFormat _timestampFormat;
};

}  // namespace mongo::logv2
