/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <vector>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class Document;
struct ExpressionContext;
class Value;

/**
 * This stage takes a stream of input documents and makes them available to multiple consumers. To
 * do so, it will buffer all incoming documents up to the configured memory limit, then provide
 * access to that buffer via an iterator.
 *
 * TODO SERVER-24153: This stage should be able to spill to disk if allowed to and the memory limit
 * has been exceeded.
 */
class TeeBuffer : public RefCountable {
public:
    using const_iterator = std::vector<Document>::const_iterator;

    static const uint64_t kMaxMemoryUsageBytes = 100 * 1024 * 1024;

    static boost::intrusive_ptr<TeeBuffer> create(
        uint64_t maxMemoryUsageBytes = kMaxMemoryUsageBytes);

    void setSource(const boost::intrusive_ptr<DocumentSource>& source) {
        _source = source;
    }

    /**
     * Clears '_buffer'. Once dispose() is called, all iterators are invalid, and it is illegal to
     * call begin() or end().
     */
    void dispose();

    /**
     * Populates the buffer by consuming all input from 'pSource'. This must be called before
     * calling begin() or end().
     */
    void populate();

    const_iterator begin() const;
    const_iterator end() const;

private:
    TeeBuffer(uint64_t maxMemoryUsageBytes);

    bool _populated = false;
    uint64_t _maxMemoryUsageBytes;
    std::vector<Document> _buffer;
    boost::intrusive_ptr<DocumentSource> _source;
};
}  // namespace mongo
