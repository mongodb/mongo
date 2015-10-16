/*    Copyright 2012 10gen Inc.
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

#include "mongo/base/status.h"
#include "mongo/util/mongoutils/str.h"

#include <ostream>
#include <sstream>

namespace mongo {

Status::ErrorInfo::ErrorInfo(ErrorCodes::Error aCode, std::string aReason, int aLocation)
    : code(aCode), reason(std::move(aReason)), location(aLocation) {}

Status::ErrorInfo* Status::ErrorInfo::create(ErrorCodes::Error c, std::string r, int l) {
    if (c == ErrorCodes::OK)
        return nullptr;
    return new ErrorInfo(c, std::move(r), l);
}

Status::Status(ErrorCodes::Error code, std::string reason, int location)
    : _error(ErrorInfo::create(code, std::move(reason), location)) {
    ref(_error);
}

Status::Status(ErrorCodes::Error code, const char* reason, int location)
    : Status(code, std::string(reason), location) {}

Status::Status(ErrorCodes::Error code, const mongoutils::str::stream& reason, int location)
    : Status(code, std::string(reason), location) {}

bool Status::compare(const Status& other) const {
    return code() == other.code() && location() == other.location();
}

bool Status::operator==(const Status& other) const {
    return compare(other);
}

bool Status::operator!=(const Status& other) const {
    return !compare(other);
}

bool Status::compareCode(const ErrorCodes::Error other) const {
    return code() == other;
}

bool Status::operator==(const ErrorCodes::Error other) const {
    return compareCode(other);
}

bool Status::operator!=(const ErrorCodes::Error other) const {
    return !compareCode(other);
}

std::ostream& operator<<(std::ostream& os, const Status& status) {
    return os << status.codeString() << " " << status.reason();
}

std::ostream& operator<<(std::ostream& os, ErrorCodes::Error code) {
    return os << ErrorCodes::errorString(code);
}

std::string Status::toString() const {
    std::ostringstream ss;
    ss << codeString();
    if (!isOK())
        ss << ": " << reason();
    if (location() != 0)
        ss << " @ " << location();
    return ss.str();
}

}  // namespace mongo
