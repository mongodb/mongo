/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/auth/cluster_auth_mode.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {
constexpr auto kKeyFileStr = "keyFile"_sd;
constexpr auto kSendKeyFileStr = "sendKeyFile"_sd;
constexpr auto kSendX509Str = "sendX509"_sd;
constexpr auto kX509Str = "x509"_sd;
constexpr auto kUndefinedStr = "undefined"_sd;
}  // namespace

StatusWith<ClusterAuthMode> ClusterAuthMode::parse(StringData strMode) {
    if (strMode == kKeyFileStr) {
        return ClusterAuthMode(Value::kKeyFile);
    } else if (strMode == kSendKeyFileStr) {
        return ClusterAuthMode(Value::kSendKeyFile);
    } else if (strMode == kSendX509Str) {
        return ClusterAuthMode(Value::kSendX509);
    } else if (strMode == kX509Str) {
        return ClusterAuthMode(Value::kX509);
    } else {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid clusterAuthMode '" << strMode << "'");
    }

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::isDefined() const {
    switch (_value) {
        case Value::kUndefined:
            return false;
        case Value::kX509:
        case Value::kKeyFile:
        case Value::kSendKeyFile:
        case Value::kSendX509:
            return true;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::canTransitionTo(const ClusterAuthMode& mode) const {
    switch (_value) {
        case Value::kUndefined:
            return true;
        case Value::kKeyFile:
            return mode._value == Value::kSendKeyFile;
        case Value::kSendKeyFile:
            return mode._value == Value::kSendX509;
        case Value::kSendX509:
            return mode._value == Value::kX509;
        case Value::kX509:
            return false;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::keyFileOnly() const {
    switch (_value) {
        case Value::kUndefined:
        case Value::kSendKeyFile:
        case Value::kSendX509:
        case Value::kX509:
            return false;
        case Value::kKeyFile:
            return true;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::allowsKeyFile() const {
    switch (_value) {
        case Value::kUndefined:
        case Value::kX509:
            return false;
        case Value::kKeyFile:
        case Value::kSendKeyFile:
        case Value::kSendX509:
            return true;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::allowsX509() const {
    switch (_value) {
        case Value::kUndefined:
        case Value::kKeyFile:
            return false;
        case Value::kSendKeyFile:
        case Value::kSendX509:
        case Value::kX509:
            return true;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::sendsKeyFile() const {
    switch (_value) {
        case Value::kUndefined:
        case Value::kSendX509:
        case Value::kX509:
            return false;
        case Value::kKeyFile:
        case Value::kSendKeyFile:
            return true;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::sendsX509() const {
    switch (_value) {
        case Value::kUndefined:
        case Value::kKeyFile:
        case Value::kSendKeyFile:
            return false;
        case Value::kSendX509:
        case Value::kX509:
            return true;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::x509Only() const {
    switch (_value) {
        case Value::kUndefined:
        case Value::kKeyFile:
        case Value::kSendKeyFile:
        case Value::kSendX509:
            return false;
        case Value::kX509:
            return true;
    };

    MONGO_UNREACHABLE;
}

StringData ClusterAuthMode::toString() const {
    switch (_value) {
        case Value::kUndefined:
            return kUndefinedStr;
        case Value::kKeyFile:
            return kKeyFileStr;
        case Value::kSendKeyFile:
            return kSendKeyFileStr;
        case Value::kSendX509:
            return kSendX509Str;
        case Value::kX509:
            return kX509Str;
    };

    MONGO_UNREACHABLE;
}

bool ClusterAuthMode::equals(ClusterAuthMode& rhs) const {
    return _value == rhs._value;
}

}  // namespace mongo
