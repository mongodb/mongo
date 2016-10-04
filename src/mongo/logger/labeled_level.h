/*    Copyright 2009 10gen Inc.
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

#include <string>

#include "mongo/logger/log_severity.h"

namespace mongo {
namespace logger {

/**
 * Deprecated utility for associating a std::string and log level together.
 */
class LabeledLevel {
public:
    LabeledLevel(int level) : _level(level) {}
    LabeledLevel(const char* label, int level) : _label(label), _level(level) {}
    LabeledLevel(const std::string& label, int level) : _label(label), _level(level) {}

    LabeledLevel operator+(int i) const {
        return LabeledLevel(_label, _level + i);
    }

    LabeledLevel operator-(int i) const {
        return LabeledLevel(_label, _level - i);
    }

    const std::string& getLabel() const {
        return _label;
    }
    int getLevel() const {
        return _level;
    }

    operator LogSeverity() const {
        return logger::LogSeverity::cast(_level);
    }

private:
    std::string _label;
    int _level;
};

inline bool operator<(const LabeledLevel& ll, const int i) {
    return ll.getLevel() < i;
}
inline bool operator<(const int i, const LabeledLevel& ll) {
    return i < ll.getLevel();
}
inline bool operator>(const LabeledLevel& ll, const int i) {
    return ll.getLevel() > i;
}
inline bool operator>(const int i, const LabeledLevel& ll) {
    return i > ll.getLevel();
}
inline bool operator<=(const LabeledLevel& ll, const int i) {
    return ll.getLevel() <= i;
}
inline bool operator<=(const int i, const LabeledLevel& ll) {
    return i <= ll.getLevel();
}
inline bool operator>=(const LabeledLevel& ll, const int i) {
    return ll.getLevel() >= i;
}
inline bool operator>=(const int i, const LabeledLevel& ll) {
    return i >= ll.getLevel();
}
inline bool operator==(const LabeledLevel& ll, const int i) {
    return ll.getLevel() == i;
}
inline bool operator==(const int i, const LabeledLevel& ll) {
    return i == ll.getLevel();
}

}  // namespace logger
}  // namespace mongo
