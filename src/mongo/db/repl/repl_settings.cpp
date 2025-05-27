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


#include "mongo/db/repl/repl_settings.h"

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {


std::string ReplSettings::ourSetName() const {
    invariant(!_isServerless);
    size_t sl = _replSetString.find('/');
    if (sl == std::string::npos)
        return _replSetString;
    return _replSetString.substr(0, sl);
}

bool ReplSettings::isReplSet() const {
    return _isServerless || !_replSetString.empty() || _shouldAutoInitiate;
}

/**
 * Getters
 */

long long ReplSettings::getOplogSizeBytes() const {
    return _oplogSizeBytes;
}

std::string ReplSettings::getReplSetString() const {
    invariant(!_isServerless);
    return _replSetString;
}

bool ReplSettings::isServerless() const {
    return _isServerless;
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

void ReplSettings::setReplSetString(std::string replSetString) {
    invariant(!_isServerless);
    _replSetString = std::move(replSetString);
}

void ReplSettings::setServerlessMode() {
    invariant(_replSetString.empty());
    _isServerless = true;
}

void ReplSettings::setShouldAutoInitiate() {
    invariant(!_isServerless);
    _shouldAutoInitiate = true;
}

}  // namespace repl
}  // namespace mongo
