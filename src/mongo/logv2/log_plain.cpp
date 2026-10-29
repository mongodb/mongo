// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"

#include <string_view>

namespace mongo::logv2 {

void plainLogBypass(std::string_view message) {
    LogOptions options{LogComponent::kDefault};
    // Open a record using plain formatting.
    detail::doLogImpl(0, LogSeverity::Log(), options, message, TypeErasedAttributeStorage(), true);
}

}  // namespace mongo::logv2
