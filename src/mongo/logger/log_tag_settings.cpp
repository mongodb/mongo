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

#include "mongo/logger/log_tag_settings.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace logger {

    LogTagSettings::LogTagSettings() {
        _minimumLoggedSeverity[LogTag::kDefault] = char(LogSeverity::Log().toInt());

        for (int i = 0; i < int(LogTag::kNumLogTags); ++i) {
            _minimumLoggedSeverity[i] = _minimumLoggedSeverity[LogTag::kDefault];
            _hasMinimumLoggedSeverity[i] = false;
        }

        _hasMinimumLoggedSeverity[LogTag::kDefault] = true;
    }

    LogTagSettings::~LogTagSettings() { }

    bool LogTagSettings::hasMinimumLogSeverity(LogTag tag) const {
        dassert(int(tag) >= 0 && int(tag) < LogTag::kNumLogTags);
        return _hasMinimumLoggedSeverity[tag];
    }

    LogSeverity LogTagSettings::getMinimumLogSeverity(LogTag tag) const {
        dassert(int(tag) >= 0 && int(tag) < LogTag::kNumLogTags);
        return LogSeverity::cast(_minimumLoggedSeverity[tag]);
    }

    void LogTagSettings::setMinimumLoggedSeverity(LogTag tag, LogSeverity severity) {
        dassert(int(tag) >= 0 && int(tag) < LogTag::kNumLogTags);
        _minimumLoggedSeverity[tag] = char(severity.toInt());
        _hasMinimumLoggedSeverity[tag] = true;

        // Set severities for unconfigured tags to be the same as LogTag::kDefault.
        if (tag == LogTag::kDefault) {
            for (int i = 0; i < int(LogTag::kNumLogTags); ++i) {
                if (!_hasMinimumLoggedSeverity[i]) {
                    _minimumLoggedSeverity[i] = char(severity.toInt());
                }
            }
        }
    }

    void LogTagSettings::clearMinimumLoggedSeverity(LogTag tag) {
        dassert(int(tag) >= 0 && int(tag) < LogTag::kNumLogTags);

        // LogTag::kDefault must always be configured.
        if (tag == LogTag::kDefault) {
            setMinimumLoggedSeverity(tag, LogSeverity::Log());
            return;
        }

        // Set unconfigured severity level to match LogTag::kDefault.
        _minimumLoggedSeverity[tag] = _minimumLoggedSeverity[LogTag::kDefault];
        _hasMinimumLoggedSeverity[tag] = false;
    }

    bool LogTagSettings::shouldLog(LogSeverity severity) const {
        return severity >= LogSeverity::cast(_minimumLoggedSeverity[LogTag::kDefault]);
    }

    bool LogTagSettings::shouldLog(LogTag tag, LogSeverity severity) const {
        dassert(int(tag) >= 0 && int(tag) < LogTag::kNumLogTags);

        // Should match LogTag::kDefault if minimum severity level is not configured for tag.
        dassert(_hasMinimumLoggedSeverity[tag] ||
                  _minimumLoggedSeverity[tag] ==
                      _minimumLoggedSeverity[LogTag::kDefault]);

        return severity >= LogSeverity::cast(_minimumLoggedSeverity[tag]);
    }

}  // logger
}  // mongo
