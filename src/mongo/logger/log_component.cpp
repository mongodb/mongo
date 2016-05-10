/*    Copyright 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/logger/log_component.h"


#include "mongo/base/init.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace logger {

namespace {

// Component dotted names.
// Lazily evaluated in LogComponent::getDottedName().
std::string _dottedNames[LogComponent::kNumLogComponents + 1];

//
// Fully initialize _dottedNames before we enter multithreaded execution.
//

MONGO_INITIALIZER_WITH_PREREQUISITES(SetupDottedNames, MONGO_NO_PREREQUISITES)
(InitializerContext* context) {
    for (int i = 0; i <= int(LogComponent::kNumLogComponents); ++i) {
        logger::LogComponent component = static_cast<logger::LogComponent::Value>(i);
        component.getDottedName();
    }

    return Status::OK();
}

}  // namespace

// Children always come after parent component.
// This makes it unnecessary to compute children of each component
// when setting/clearing log severities in LogComponentSettings.
#define DECLARE_LOG_COMPONENT_PARENT(CHILD, PARENT)                              \
    case (CHILD):                                                                \
        do {                                                                     \
            static_assert(int(CHILD) > int(PARENT), "int(CHILD) > int(PARENT)"); \
            return (PARENT);                                                     \
        } while (0)

LogComponent LogComponent::parent() const {
    switch (_value) {
        case kDefault:
            return kNumLogComponents;
            DECLARE_LOG_COMPONENT_PARENT(kJournal, kStorage);
            DECLARE_LOG_COMPONENT_PARENT(kASIO, kNetwork);
            DECLARE_LOG_COMPONENT_PARENT(kBridge, kNetwork);
        case kNumLogComponents:
            return kNumLogComponents;
        default:
            return kDefault;
    }
    invariant(false);
}

StringData LogComponent::toStringData() const {
    switch (_value) {
        case kDefault:
            return "default"_sd;
        case kAccessControl:
            return "accessControl"_sd;
        case kCommand:
            return "command"_sd;
        case kControl:
            return "control"_sd;
        case kExecutor:
            return "executor"_sd;
        case kGeo:
            return "geo"_sd;
        case kIndex:
            return "index"_sd;
        case kNetwork:
            return "network"_sd;
        case kQuery:
            return "query"_sd;
        case kReplication:
            return "replication"_sd;
        case kSharding:
            return "sharding"_sd;
        case kStorage:
            return "storage"_sd;
        case kJournal:
            return "journal"_sd;
        case kWrite:
            return "write"_sd;
        case kFTDC:
            return "ftdc"_sd;
        case kASIO:
            return "asio"_sd;
        case kBridge:
            return "bridge"_sd;
        case kNumLogComponents:
            return "total"_sd;
            // No default. Compiler should complain if there's a log component that's not handled.
    }
    invariant(false);
}

std::string LogComponent::getShortName() const {
    return toStringData().toString();
}

std::string LogComponent::getDottedName() const {
    // Lazily evaluate dotted names in anonymous namespace.
    if (_dottedNames[_value].empty()) {
        switch (_value) {
            case kDefault:
                _dottedNames[_value] = getShortName();
                break;
            case kNumLogComponents:
                _dottedNames[_value] = getShortName();
                break;
            default:
                // Omit short name of 'default' component from dotted name.
                if (parent() == kDefault) {
                    _dottedNames[_value] = getShortName();
                } else {
                    _dottedNames[_value] = parent().getDottedName() + "." + getShortName();
                }
                break;
        }
    }
    return _dottedNames[_value];
}

StringData LogComponent::getNameForLog() const {
    switch (_value) {
        case kDefault:
            return "-       "_sd;
        case kAccessControl:
            return "ACCESS  "_sd;
        case kCommand:
            return "COMMAND "_sd;
        case kControl:
            return "CONTROL "_sd;
        case kExecutor:
            return "EXECUTOR"_sd;
        case kGeo:
            return "GEO     "_sd;
        case kIndex:
            return "INDEX   "_sd;
        case kNetwork:
            return "NETWORK "_sd;
        case kQuery:
            return "QUERY   "_sd;
        case kReplication:
            return "REPL    "_sd;
        case kSharding:
            return "SHARDING"_sd;
        case kStorage:
            return "STORAGE "_sd;
        case kJournal:
            return "JOURNAL "_sd;
        case kWrite:
            return "WRITE   "_sd;
        case kFTDC:
            return "FTDC    "_sd;
        case kASIO:
            return "ASIO    "_sd;
        case kBridge:
            return "BRIDGE  "_sd;
        case kNumLogComponents:
            return "TOTAL   "_sd;
            // No default. Compiler should complain if there's a log component that's not handled.
    }
    invariant(false);
}

std::ostream& operator<<(std::ostream& os, LogComponent component) {
    return os << component.getNameForLog();
}

}  // logger
}  // mongo
