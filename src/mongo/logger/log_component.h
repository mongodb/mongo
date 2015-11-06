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

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/base/string_data.h"

namespace mongo {
namespace logger {

/**
 * Log components.
 * Debug messages logged using the LOG() or MONGO_LOG_COMPONENT().
 * Macros may be associated with one or more log components.
 */
class LogComponent {
public:
    enum Value {
        kDefault = 0,
        kAccessControl,
        kCommand,
        kControl,
        kExecutor,
        kGeo,
        kIndex,
        kNetwork,
        kQuery,
        kReplication,
        kSharding,
        kStorage,
        kJournal,
        kWrite,
        kFTDC,
        kASIO,
        kBridge,
        kNumLogComponents
    };

    /* implicit */ LogComponent(Value value) : _value(value) {}

    operator Value() const {
        return _value;
    }

    /**
     * Returns parent component.
     * Returns kNumComponents if parent component is not defined (for kDefault or
     * kNumLogComponents).
     */
    LogComponent parent() const;

    /**
     * Returns short name as a StringData.
     */
    StringData toStringData() const;

    /**
     * Returns short name of log component.
     * Used to generate server parameter names in the format "logLevel_<component short name>".
     */
    std::string getShortName() const;

    /**
     * Returns dotted name of log component - short name prefixed by dot-separated names of
     * ancestors.
     * Used to generate command line and config file option names.
     */
    std::string getDottedName() const;

    /**
     * Returns name suitable for inclusion in formatted log message.
     * This is derived from upper-casing the short name with some padding to
     * fit into a fixed length field.
     */
    StringData getNameForLog() const;

private:
    Value _value;
};

std::ostream& operator<<(std::ostream& os, LogComponent component);

}  // namespace logger
}  // namespace mongo
