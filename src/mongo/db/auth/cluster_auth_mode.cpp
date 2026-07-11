// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/auth/cluster_auth_mode.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/move/utility_core.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAccessControl


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
constexpr auto kKeyFileStr = "keyFile"sv;
constexpr auto kSendKeyFileStr = "sendKeyFile"sv;
constexpr auto kSendX509Str = "sendX509"sv;
constexpr auto kX509Str = "x509"sv;
constexpr auto kUndefinedStr = "undefined"sv;
}  // namespace

StatusWith<ClusterAuthMode> ClusterAuthMode::parse(std::string_view strMode) {
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

std::string_view ClusterAuthMode::toString() const {
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
