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

#include "mongo/platform/basic.h"

#include "mongo/rpc/document_range.h"

#include <algorithm>
#include <cstring>
#include <tuple>
#include <utility>

#include "mongo/base/data_type_validated.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

DocumentRange::DocumentRange(const char* begin, const char* end) : _range{begin, end} {}

DocumentRange::const_iterator DocumentRange::begin() const {
    return const_iterator{ConstDataRangeCursor{_range}};
}

DocumentRange::const_iterator DocumentRange::end() const {
    return const_iterator{};
}

ConstDataRange DocumentRange::data() const {
    return _range;
}

bool operator==(const DocumentRange& lhs, const DocumentRange& rhs) {
    // We might want to change this to use std::equal in the future if
    // we ever allow non-contigious document ranges
    return (lhs._range.length() == rhs._range.length()) &&
        (std::memcmp(lhs._range.data(), rhs._range.data(), lhs._range.length()) == 0);
}

bool operator!=(const DocumentRange& lhs, const DocumentRange& rhs) {
    return !(lhs == rhs);
}

DocumentRange::const_iterator::const_iterator(ConstDataRangeCursor cursor) : _cursor{cursor} {
    operator++();
}

DocumentRange::const_iterator::reference DocumentRange::const_iterator::operator*() const {
    return _obj;
}

DocumentRange::const_iterator::pointer DocumentRange::const_iterator::operator->() const {
    return &_obj;
}

DocumentRange::const_iterator& DocumentRange::const_iterator::operator++() {
    if (_cursor.length() == 0) {
        *this = const_iterator{};
    } else {
        _obj = uassertStatusOK(_cursor.readAndAdvance<Validated<BSONObj>>()).val;
    }
    return *this;
}

DocumentRange::const_iterator DocumentRange::const_iterator::operator++(int) {
    auto pre = const_iterator{_cursor};
    operator++();
    return pre;
}

bool operator==(const DocumentRange::const_iterator& lhs,
                const DocumentRange::const_iterator& rhs) {
    return lhs._cursor == rhs._cursor;
}

bool operator!=(const DocumentRange::const_iterator& lhs,
                const DocumentRange::const_iterator& rhs) {
    return !(lhs == rhs);
}

}  // namespace rpc
}  // namespace mongo
