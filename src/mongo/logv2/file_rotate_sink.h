// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/logv2/log_format.h"
#include "mongo/util/modules.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <boost/log/core/record_view.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>

namespace mongo::logv2 {
// boost::log backend sink to provide MongoDB style file rotation.
// Uses custom stream type to open log files with shared access on Windows, somthing the built-in
// boost file rotation sink does not do.
class FileRotateSink : public boost::log::sinks::text_ostream_backend {
public:
    FileRotateSink(LogTimestampFormat timestampFormat);
    ~FileRotateSink();

    Status addFile(const std::string& filename, bool append);
    void removeFile(const std::string& filename);

    Status rotate(bool rename,
                  std::string_view renameSuffix,
                  std::function<void(Status)> onMinorError);

    void consume(const boost::log::record_view& rec, const string_type& formatted_string);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo::logv2
