// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

namespace mongo {
namespace audit {

enum class AuditFormat {
    AuditFormatJsonFile = 0,
    AuditFormatBsonFile = 1,
    AuditFormatConsole = 2,
    AuditFormatSyslog = 3,
    AuditFormatMock = 4,
};

}  // namespace audit
}  // namespace mongo
