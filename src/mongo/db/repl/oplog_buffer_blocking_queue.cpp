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

#include "mongo/db/repl/oplog_buffer_blocking_queue.h"

namespace mongo {
namespace repl {

namespace {

// Limit buffer to 256MB
const size_t kOplogBufferSize = 256 * 1024 * 1024;

size_t getDocumentSize(const BSONObj& o) {
    // SERVER-9808 Avoid Fortify complaint about implicit signed->unsigned conversion
    return static_cast<size_t>(o.objsize());
}

}  // namespace

OplogBufferBlockingQueue::OplogBufferBlockingQueue() : _queue(kOplogBufferSize, &getDocumentSize) {}

void OplogBufferBlockingQueue::startup() {}

void OplogBufferBlockingQueue::shutdown() {
    clear();
}

bool OplogBufferBlockingQueue::pushAllNonBlocking(Batch::const_iterator begin,
                                                  Batch::const_iterator end) {
    _queue.pushAllNonBlocking(begin, end);
    return true;
}

void OplogBufferBlockingQueue::waitForSpace(std::size_t size) {
    _queue.waitForSpace(size);
}

std::size_t OplogBufferBlockingQueue::getSize() const {
    return _queue.size();
}

std::size_t OplogBufferBlockingQueue::getCount() const {
    return _queue.count();
}

void OplogBufferBlockingQueue::clear() {
    _queue.clear();
}

bool OplogBufferBlockingQueue::tryPop(Value* value) {
    return _queue.tryPop(*value);
}

bool OplogBufferBlockingQueue::peek(Value* value) {
    return _queue.peek(*value);
}

}  // namespace repl
}  // namespace mongo
