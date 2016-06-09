/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/pipeline/tee_buffer.h"

#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

TeeBuffer::TeeBuffer(uint64_t maxMemoryUsageBytes)
    : _maxMemoryUsageBytes(maxMemoryUsageBytes), _buffer() {}

boost::intrusive_ptr<TeeBuffer> TeeBuffer::create(uint64_t maxMemoryUsageBytes) {
    return new TeeBuffer(maxMemoryUsageBytes);
}

TeeBuffer::const_iterator TeeBuffer::begin() const {
    invariant(_populated);
    return _buffer.begin();
}

TeeBuffer::const_iterator TeeBuffer::end() const {
    invariant(_populated);
    return _buffer.end();
}

void TeeBuffer::dispose() {
    _buffer.clear();
    _populated = false;  // Set this to ensure no one is calling begin() or end().
}

void TeeBuffer::populate() {
    invariant(_source);
    if (_populated) {
        return;
    }
    _populated = true;

    size_t estimatedMemoryUsageBytes = 0;
    while (auto next = _source->getNext()) {
        estimatedMemoryUsageBytes += next->getApproximateSize();
        uassert(40174,
                "Exceeded memory limit for $facet",
                estimatedMemoryUsageBytes <= _maxMemoryUsageBytes);

        _buffer.emplace_back(std::move(*next));
    }
}
}  // namespace mongo
