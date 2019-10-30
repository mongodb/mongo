/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/logger/log_component.h"
#include "mongo/logger/log_severity.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"

namespace mongo {
inline logv2::LogComponent logComponentV1toV2(logger::LogComponent component) {
    return logv2::LogComponent(static_cast<logv2::LogComponent::Value>(
        static_cast<std::underlying_type_t<logger::LogComponent::Value>>(
            static_cast<logger::LogComponent::Value>(component))));
}
inline logger::LogComponent logComponentV2toV1(logv2::LogComponent component) {
    return logger::LogComponent(static_cast<logger::LogComponent::Value>(
        static_cast<std::underlying_type_t<logv2::LogComponent::Value>>(
            static_cast<logv2::LogComponent::Value>(component))));
}
inline logv2::LogSeverity logSeverityV1toV2(logger::LogSeverity severity) {
    return logv2::LogSeverity::cast(severity.toInt());
}
inline logger::LogSeverity logSeverityV2toV1(logv2::LogSeverity severity) {
    return logger::LogSeverity::cast(severity.toInt());
}
}  // namespace mongo