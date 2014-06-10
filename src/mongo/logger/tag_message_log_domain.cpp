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

#include "mongo/logger/tag_message_log_domain.h"

namespace mongo {
namespace logger {

    TagMessageLogDomain::TagMessageLogDomain() {}

    TagMessageLogDomain::~TagMessageLogDomain() {}

    bool TagMessageLogDomain::hasMinimumLogSeverity(LogTag tag) const {
        return _settings.hasMinimumLogSeverity(tag);
    }

    bool TagMessageLogDomain::shouldLog(LogSeverity severity) const {
        return _settings.shouldLog(severity);
    }

    bool TagMessageLogDomain::shouldLog(LogTag tag, LogSeverity severity) const {
        return _settings.shouldLog(tag, severity);
    }

    bool TagMessageLogDomain::shouldLog(LogTag tag1, LogTag tag2, LogSeverity severity) const {
        return _settings.shouldLog(tag1, severity) || _settings.shouldLog(tag2, severity);
    }

    bool TagMessageLogDomain::shouldLog(LogTag tag1, LogTag tag2, LogTag tag3,
                                        LogSeverity severity) const {
        return _settings.shouldLog(tag1, severity) || _settings.shouldLog(tag2, severity) ||
               _settings.shouldLog(tag3, severity);
    }

    LogSeverity TagMessageLogDomain::getMinimumLogSeverity() const {
        return _settings.getMinimumLogSeverity(LogTag::kDefault);
    }

    LogSeverity TagMessageLogDomain::getMinimumLogSeverity(LogTag tag) const {
        return _settings.getMinimumLogSeverity(tag);
    }

    void TagMessageLogDomain::setMinimumLoggedSeverity(LogSeverity severity) {
        _settings.setMinimumLoggedSeverity(LogTag::kDefault, severity);
    }

    void TagMessageLogDomain::setMinimumLoggedSeverity(LogTag tag, LogSeverity severity) {
        _settings.setMinimumLoggedSeverity(tag, severity);
    }

    void TagMessageLogDomain::clearMinimumLoggedSeverity(LogTag tag) {
        _settings.clearMinimumLoggedSeverity(tag);
    }

}  // namespace logger
}  // namespace mongo
