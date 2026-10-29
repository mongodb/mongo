// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/repl/repl_settings.h"

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {


std::string ReplSettings::ourSetName() const {
    size_t sl = _replSetString.find('/');
    if (sl == std::string::npos)
        return _replSetString;
    return _replSetString.substr(0, sl);
}

bool ReplSettings::isReplSet() const {
    return !_replSetString.empty() || _shouldAutoInitiate;
}

/**
 * Getters
 */

long long ReplSettings::getOplogSizeBytes() const {
    return _oplogSizeBytes;
}

bool ReplSettings::isOplogSizeInitializedUsingDefault() const {
    return _oplogSizeInitializedUsingDefault;
}

std::string ReplSettings::getReplSetString() const {
    return _replSetString;
}

bool ReplSettings::shouldRecoverFromOplogAsStandalone() {
    return recoverFromOplogAsStandalone;
}

bool ReplSettings::shouldSkipOplogSampling() {
    return skipOplogSampling;
}

bool ReplSettings::shouldAutoInitiate() const {
    return _shouldAutoInitiate;
}

/**
 * Setters
 */

void ReplSettings::setOplogSizeBytes(long long oplogSizeBytes) {
    _oplogSizeBytes = oplogSizeBytes;
}

void ReplSettings::setOplogSizeInitializedUsingDefault(bool value) {
    _oplogSizeInitializedUsingDefault = value;
}

void ReplSettings::setReplSetString(std::string replSetString) {
    _replSetString = std::move(replSetString);
}

void ReplSettings::setShouldAutoInitiate() {
    _shouldAutoInitiate = true;
}

}  // namespace repl
}  // namespace mongo
