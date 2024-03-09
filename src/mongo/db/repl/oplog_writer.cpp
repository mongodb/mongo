/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/repl/oplog_writer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

NoopOplogWriterObserver noopOplogWriterObserver;

OplogWriter::OplogWriter(executor::TaskExecutor* executor,
                         OplogBuffer* writeBuffer,
                         const Options& options)
    : _batcher(writeBuffer), _executor(executor), _writeBuffer(writeBuffer), _options(options) {}

OplogBuffer* OplogWriter::getBuffer() const {
    return _writeBuffer;
}

Future<void> OplogWriter::startup() {
    auto pf = makePromiseFuture<void>();

    auto callback = [this, promise = std::move(pf.promise)](
                        const executor::TaskExecutor::CallbackArgs& args) mutable noexcept {
        invariant(args.status);
        LOGV2(8543100, "Starting oplog write");
        _run();
        LOGV2(8543101, "Finished oplog write");
        promise.setWith([] {});
    };
    invariant(_executor->scheduleWork(std::move(callback)).getStatus());

    return std::move(pf.future);
}

void OplogWriter::shutdown() {
    stdx::lock_guard<Latch> lock(_mutex);
    _inShutdown = true;
}

bool OplogWriter::inShutdown() const {
    stdx::lock_guard<Latch> lock(_mutex);
    return _inShutdown;
}

void OplogWriter::enqueue(OperationContext* opCtx,
                          OplogBuffer::Batch::const_iterator begin,
                          OplogBuffer::Batch::const_iterator end,
                          boost::optional<std::size_t> bytes) {
    static Occasionally sampler;
    if (sampler.tick()) {
        LOGV2_DEBUG(8569804,
                    2,
                    "Oplog write buffer size",
                    "oplogWriteBufferSizeBytes"_attr = _writeBuffer->getSize());
    }
    _writeBuffer->push(opCtx, begin, end, bytes);
}

const OplogWriter::Options& OplogWriter::getOptions() const {
    return _options;
}

}  // namespace repl
}  // namespace mongo
