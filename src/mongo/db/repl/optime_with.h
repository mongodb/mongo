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

#pragma once

#include <iosfwd>
#include <string>

#include "mongo/db/repl/optime.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

/**
 * Basic structure that contains a value and an opTime.
 */
template <typename T>
struct OpTimeWith {
public:
    OpTimeWith() = default;
    explicit OpTimeWith(T val) : value(std::move(val)) {}
    OpTimeWith(T val, repl::OpTime ts) : value(std::move(val)), opTime(std::move(ts)) {}

    std::string toString() const;

    bool operator==(const OpTimeWith<T>& rhs) const {
        return opTime == rhs.opTime && value == rhs.value;
    }

    T value;
    repl::OpTime opTime;
};

template <typename T>
std::ostream& operator<<(std::ostream& out, const OpTimeWith<T>& opTimeWith) {
    return out << opTimeWith.toString();
}

template <typename T>
std::string OpTimeWith<T>::toString() const {
    return str::stream() << opTime.toString() << "[" << value << "]";
}

}  // namespace repl
}  // namespace mongo
