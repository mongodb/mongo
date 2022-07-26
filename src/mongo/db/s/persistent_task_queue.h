/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/stdx/condition_variable.h"

namespace mongo {

using TaskId = std::int64_t;

// A BlockingTaskQueue is a FIFO container for elements that implements our IDL serialization
// interface. The concurrency model that is supported is multiple producer/single-consumer. This is
// because of the relationship between the peek and pop calls.
template <typename T>
class BlockingTaskQueue {
public:
    virtual ~BlockingTaskQueue() {}

    struct Record {
        Record(TaskId id, T task) : id(id), task(std::move(task)) {}

        TaskId id;
        T task;
    };

    // Enqueue the passed in element to the end of the queue and return a unique id that
    // represents this item.
    virtual TaskId push(OperationContext* opCtx, const T& t) = 0;

    // Remove the first element from the queue. This can only be called after a
    // successful call to peek().
    virtual TaskId pop(OperationContext* opCtx) = 0;

    // Returns the first element of the queue. It will block if the queue is empty. It
    // will only be unblocked by a call to push or a call to interrupt.
    virtual const Record& peek(OperationContext* opCtx) = 0;

    // This will mark the queue as closed. Any currently blocked peek() operation will
    // be unblocked and will throw an exception indicating that it was closed. Any
    // subsequent calls to push, pop or peek will throw an Interrupted exception.
    virtual void close(OperationContext* opCtx) = 0;

    virtual size_t size(OperationContext* opCtx) const = 0;
    virtual bool empty(OperationContext* opCtx) const = 0;
};

// PersistentTaskQueue is an implementation of the BlockingTaskQueue interface that supports the
// persistence of its internal storage making it durable across instantiations.
template <typename T>
class PersistentTaskQueue final : public BlockingTaskQueue<T> {
public:
    PersistentTaskQueue(OperationContext* opCtx, NamespaceString storageNss);

    TaskId push(OperationContext* opCtx, const T& t) override;
    TaskId pop(OperationContext* opCtx) override;
    const typename BlockingTaskQueue<T>::Record& peek(OperationContext* opCtx) override;
    void close(OperationContext* opCtx) override;

    size_t size(OperationContext* opCtx) const override;
    bool empty(OperationContext* opCtx) const override;

private:
    TaskId _loadLastId(DBDirectClient& client);
    boost::optional<typename BlockingTaskQueue<T>::Record> _loadNextRecord(DBDirectClient& client);

    NamespaceString _storageNss;
    size_t _count{0};
    bool _closed{false};
    TaskId _lastId{0};
    boost::optional<typename BlockingTaskQueue<T>::Record> _currentFront;

    Lock::ResourceMutex _mutex;
    stdx::condition_variable_any _cv;
};

template <typename T>
PersistentTaskQueue<T>::PersistentTaskQueue(OperationContext* opCtx, NamespaceString storageNss)
    : _storageNss(std::move(storageNss)), _mutex("persistentQueueLock:" + _storageNss.toString()) {

    DBDirectClient client(opCtx);

    FindCommandRequest findRequest{_storageNss};
    findRequest.setProjection(BSON("_id" << 1));
    auto cursor = client.find(std::move(findRequest));
    _count = cursor->itcount();

    if (_count > 0)
        _lastId = _loadLastId(client);
}

template <typename T>
TaskId PersistentTaskQueue<T>::push(OperationContext* opCtx, const T& t) {
    DBDirectClient dbClient(opCtx);

    TaskId recordId = 0;
    BSONObjBuilder builder;

    {
        Lock::ExclusiveLock lock(opCtx->lockState(), _mutex);

        uassert(ErrorCodes::Interrupted, "Task queue was closed", !_closed);

        recordId = ++_lastId;
        builder.append("_id", recordId);
        builder.append("task", t.toBSON());

        auto response = write_ops::checkWriteErrors(
            dbClient.insert(write_ops::InsertCommandRequest(_storageNss, {builder.obj()})));
        _count++;
    }

    _cv.notify_one();

    return recordId;
}

template <typename T>
TaskId PersistentTaskQueue<T>::pop(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    BSONObjBuilder builder;

    Lock::ExclusiveLock lock(opCtx->lockState(), _mutex);

    uassert(ErrorCodes::Interrupted, "Task queue was closed", !_closed);

    uassert(31294, "peek() must be called before pop().", _currentFront);
    invariant(_count > 0);

    builder.append("_id", _currentFront->id);

    write_ops::DeleteCommandRequest deleteOp(_storageNss);
    deleteOp.setDeletes({write_ops::DeleteOpEntry(builder.obj(), false)});
    write_ops::checkWriteErrors(client.remove(deleteOp));
    _count--;

    TaskId id = _currentFront->id;
    _currentFront.reset();

    return id;
}

template <typename T>
const typename BlockingTaskQueue<T>::Record& PersistentTaskQueue<T>::peek(OperationContext* opCtx) {
    DBDirectClient client(opCtx);

    Lock::ExclusiveLock lock(opCtx->lockState(), _mutex);

    opCtx->waitForConditionOrInterrupt(_cv, lock, [this] { return _count > 0 || _closed; });
    uassert(ErrorCodes::Interrupted, "Task queue was closed", !_closed);

    _currentFront = _loadNextRecord(client);
    uassert(ErrorCodes::InternalError, "Task queue is in an invalid state.", _currentFront);

    return *_currentFront;
}

template <typename T>
void PersistentTaskQueue<T>::close(OperationContext* opCtx) {
    Lock::ExclusiveLock lock(opCtx->lockState(), _mutex);

    _closed = true;
    _cv.notify_all();
}

template <typename T>
size_t PersistentTaskQueue<T>::size(OperationContext* opCtx) const {
    Lock::ExclusiveLock lock(opCtx->lockState(), _mutex);
    return _count;
}

template <typename T>
bool PersistentTaskQueue<T>::empty(OperationContext* opCtx) const {
    Lock::ExclusiveLock lock(opCtx->lockState(), _mutex);

    return _count == 0;
}

template <typename T>
TaskId PersistentTaskQueue<T>::_loadLastId(DBDirectClient& client) {
    FindCommandRequest findCmd{_storageNss};
    findCmd.setSort(BSON("_id" << -1));
    findCmd.setProjection(BSON("_id" << 1));
    auto maxId = client.findOne(std::move(findCmd));
    return maxId.getField("_id").Long();
}

template <typename T>
typename boost::optional<typename BlockingTaskQueue<T>::Record>
PersistentTaskQueue<T>::_loadNextRecord(DBDirectClient& client) {
    FindCommandRequest findCmd{_storageNss};
    findCmd.setSort(BSON("_id" << 1));
    auto bson = client.findOne(std::move(findCmd));

    boost::optional<typename PersistentTaskQueue<T>::Record> result;

    if (!bson.isEmpty()) {
        result = typename PersistentTaskQueue<T>::Record{
            bson.getField("_id").Long(),
            T::parse(IDLParserContext("PersistentTaskQueue:" + _storageNss.toString()),
                     bson.getObjectField("task"))};
    }

    return result;
}

}  // namespace mongo
