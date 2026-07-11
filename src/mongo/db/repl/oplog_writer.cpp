// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_writer.h"

#include "mongo/util/observable_mutex_registry.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

namespace mongo {
namespace repl {

OplogWriter::OplogWriter(executor::TaskExecutor* executor,
                         OplogBuffer* writeBuffer,
                         const Options& options)
    : _batcher(writeBuffer), _executor(executor), _writeBuffer(writeBuffer), _options(options) {
    ObservableMutexRegistry::get().add("oplogWriterMutex", _mutex);
}

OplogBuffer* OplogWriter::getBuffer() const {
    return _writeBuffer;
}

Future<void> OplogWriter::startup() {
    auto pf = makePromiseFuture<void>();

    auto callback = [this, promise = std::move(pf.promise)](
                        const executor::TaskExecutor::CallbackArgs& args) mutable noexcept {
        invariant(args.status);
        LOGV2(8543100, "Starting oplog write");
        try {
            _run();
        } catch (DBException& e) {
            LOGV2(10185800,
                  "OplogWriter threw a DBException",
                  "what"_attr = e.what(),
                  "exception"_attr = e.toString());
            fasserted(10185801);
        }
        LOGV2(8543101, "Finished oplog write");
        promise.setWith([] {});
    };
    invariant(_executor->scheduleWork(std::move(callback)).getStatus());

    return std::move(pf.future);
}

void OplogWriter::shutdown() {
    std::lock_guard lock(_mutex);
    _inShutdown = true;
}

bool OplogWriter::inShutdown() const {
    std::lock_guard lock(_mutex);
    return _inShutdown;
}

void OplogWriter::waitForSpace(OperationContext* opCtx, const OplogBuffer::Cost& cost) {
    _writeBuffer->waitForSpace(opCtx, cost);
}

void OplogWriter::enqueue(OperationContext* opCtx,
                          OplogBuffer::Batch::const_iterator begin,
                          OplogBuffer::Batch::const_iterator end,
                          const OplogBuffer::Cost& cost) {
    static Occasionally sampler;
    if (sampler.tick()) {
        LOGV2_DEBUG(8569804,
                    2,
                    "Oplog write buffer size",
                    "oplogWriteBufferSizeBytes"_attr = _writeBuffer->getSize());
    }
    _writeBuffer->push(opCtx, begin, end, cost);
}

const OplogWriter::Options& OplogWriter::getOptions() const {
    return _options;
}

}  // namespace repl
}  // namespace mongo
