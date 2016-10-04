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

#include <cstddef>
#include <iterator>

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace rpc {

/**
 * A read-only view over a sequence of BSON documents.
 *
 * TODO:
 * - Handle document validation
 * - Currently this only supports a contiguous buffer of BSON documents,
 * in the future it should support non-contiguous buffers as well.
 */
class DocumentRange {
public:
    class const_iterator;

    DocumentRange() = default;

    DocumentRange(const char* begin, const char* end);

    const_iterator begin() const;
    const_iterator end() const;

    // Get a ConstDataRange over the underlying raw buffer.
    ConstDataRange data() const;

    // Deep equality of all documents in both ranges.
    friend bool operator==(const DocumentRange& lhs, const DocumentRange& rhs);
    friend bool operator!=(const DocumentRange& lhs, const DocumentRange& rhs);

private:
    ConstDataRange _range{nullptr, nullptr};
};

class DocumentRange::const_iterator : public std::iterator<std::forward_iterator_tag,
                                                           BSONObj,
                                                           std::ptrdiff_t,
                                                           const BSONObj*,
                                                           const BSONObj&> {
public:
    const_iterator() = default;

    reference operator*() const;
    pointer operator->() const;

    const_iterator& operator++();
    const_iterator operator++(int);

    friend bool operator==(const const_iterator&, const const_iterator&);
    friend bool operator!=(const const_iterator&, const const_iterator&);

private:
    // The only way to get a non-end iterator is from DocumentRange begin().
    friend class DocumentRange;
    explicit const_iterator(ConstDataRangeCursor cursor);

    ConstDataRangeCursor _cursor{nullptr, nullptr};
    BSONObj _obj;
};

}  // namespace rpc
}  // namespace mongo
