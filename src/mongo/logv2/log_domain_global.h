// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component_settings.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_internal.h"
#include "mongo/logv2/log_format.h"
#include "mongo/logv2/log_source.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mongo::logv2 {
class [[MONGO_MOD_PUBLIC]] LogDomainGlobal : public LogDomain::Internal {
public:
    struct ConfigurationOptions {
        enum class RotationMode { kRename, kReopen };
        enum class OpenMode { kTruncate, kAppend };

        bool consoleEnabled{true};
        bool fileEnabled{false};
        std::string filePath;
        RotationMode fileRotationMode{RotationMode::kRename};
        OpenMode fileOpenMode{OpenMode::kTruncate};
        LogTimestampFormat timestampFormat{LogTimestampFormat::kISO8601Local};
        bool syslogEnabled{false};
        int syslogFacility{-1};  // invalid facility by default, must be set
        LogFormat format{LogFormat::kDefault};
        const Atomic<int32_t>* maxAttributeSizeKB = nullptr;

        std::string backtraceFilePath;

        void makeDisabled();
    };

    LogDomainGlobal();
    ~LogDomainGlobal() override;

    LogSource& source() override;

    Status configure(ConfigurationOptions const& options);
    Status rotate(bool rename,
                  std::string_view renameSuffix,
                  std::function<void(Status)> onMinorError);

    const ConfigurationOptions& config() const;

    LogComponentSettings& settings();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}  // namespace mongo::logv2
