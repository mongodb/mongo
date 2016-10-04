/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/db/repl/repl_settings.h"

#include <string>

#include "mongo/db/repl/bgsync.h"
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
bool ReplSettings::isSlave() const {
    return _slave;
}

bool ReplSettings::isMaster() const {
    return _master;
}

bool ReplSettings::isFastSyncEnabled() const {
    return _fastSyncEnabled;
}

bool ReplSettings::isAutoResyncEnabled() const {
    return _autoResyncEnabled;
}

bool ReplSettings::isMajorityReadConcernEnabled() const {
    return _majorityReadConcernEnabled;
}

Seconds ReplSettings::getSlaveDelaySecs() const {
    return _slaveDelaySecs;
}

int ReplSettings::getPretouch() const {
    return _pretouch;
}

long long ReplSettings::getOplogSizeBytes() const {
    return _oplogSizeBytes;
}

std::string ReplSettings::getSource() const {
    return _source;
}

std::string ReplSettings::getOnly() const {
    return _only;
}

std::string ReplSettings::getReplSetString() const {
    return _replSetString;
}

ReplSettings::IndexPrefetchConfig ReplSettings::getPrefetchIndexMode() const {
    return _prefetchIndexMode;
}

bool ReplSettings::isPrefetchIndexModeSet() const {
    return _prefetchIndexMode != IndexPrefetchConfig::UNINITIALIZED;
}

/**
 * Setters
 */
void ReplSettings::setSlave(bool slave) {
    _slave = slave;
}

void ReplSettings::setMaster(bool master) {
    _master = master;
}

void ReplSettings::setFastSyncEnabled(bool fastSyncEnabled) {
    _fastSyncEnabled = fastSyncEnabled;
}

void ReplSettings::setAutoResyncEnabled(bool autoResyncEnabled) {
    _autoResyncEnabled = autoResyncEnabled;
}

void ReplSettings::setMajorityReadConcernEnabled(bool majorityReadConcernEnabled) {
    _majorityReadConcernEnabled = majorityReadConcernEnabled;
}

void ReplSettings::setSlaveDelaySecs(int slaveDelay) {
    _slaveDelaySecs = Seconds(slaveDelay);
}

void ReplSettings::setPretouch(int pretouch) {
    _pretouch = pretouch;
}

void ReplSettings::setOplogSizeBytes(long long oplogSizeBytes) {
    _oplogSizeBytes = oplogSizeBytes;
}

void ReplSettings::setSource(std::string source) {
    _source = source;
}

void ReplSettings::setOnly(std::string only) {
    _only = only;
}

void ReplSettings::setReplSetString(std::string replSetString) {
    _replSetString = replSetString;
}

void ReplSettings::setPrefetchIndexMode(std::string prefetchIndexModeString) {
    if (prefetchIndexModeString.empty()) {
        _prefetchIndexMode = IndexPrefetchConfig::UNINITIALIZED;
    } else {
        if (prefetchIndexModeString == "none")
            _prefetchIndexMode = IndexPrefetchConfig::PREFETCH_NONE;
        else if (prefetchIndexModeString == "_id_only")
            _prefetchIndexMode = IndexPrefetchConfig::PREFETCH_ID_ONLY;
        else if (prefetchIndexModeString == "all")
            _prefetchIndexMode = IndexPrefetchConfig::PREFETCH_ALL;
        else {
            _prefetchIndexMode = IndexPrefetchConfig::PREFETCH_ALL;
            warning() << "unrecognized indexPrefetchMode setting \"" << prefetchIndexModeString
                      << "\", defaulting to \"all\"";
        }
    }
}

}  // namespace repl
}  // namespace mongo
