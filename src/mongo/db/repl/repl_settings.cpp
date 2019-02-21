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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/repl_settings.h"

#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_settings_gen.h"
#include "mongo/util/log.h"

namespace mongo {
namespace repl {


std::string ReplSettings::ourSetName() const {
    size_t sl = _replSetString.find('/');
    if (sl == std::string::npos)
        return _replSetString;
    return _replSetString.substr(0, sl);
}

bool ReplSettings::usingReplSets() const {
    return !_replSetString.empty();
}

/**
 * Getters
 */

long long ReplSettings::getOplogSizeBytes() const {
    return _oplogSizeBytes;
}

std::string ReplSettings::getReplSetString() const {
    return _replSetString;
}

bool ReplSettings::shouldRecoverFromOplogAsStandalone() {
    return recoverFromOplogAsStandalone;
}

/**
 * Setters
 */

void ReplSettings::setOplogSizeBytes(long long oplogSizeBytes) {
    _oplogSizeBytes = oplogSizeBytes;
}

void ReplSettings::setReplSetString(std::string replSetString) {
    _replSetString = replSetString;
}

}  // namespace repl
}  // namespace mongo
