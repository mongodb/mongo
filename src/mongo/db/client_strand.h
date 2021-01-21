/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <string>

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/out_of_line_executor.h"

namespace mongo {

/**
 * ClientStrand is a reference counted type for loaning Clients to threads.
 *
 * ClientStrand maintains the lifetime of its wrapped Client object and provides functionality to
 * "bind" that Client to one and only one thread at a time. Its functions are synchronized.
 */
class ClientStrand final : public RefCountable {
    static constexpr auto kDiagnosticLogLevel = 3;

public:
    static constexpr auto kUnableToRecoverClient = "Unable to recover Client for ClientStrand";

    /**
     * A simple RAII guard to set and release Clients.
     */
    class Guard {
    public:
        Guard() = default;
        Guard(Guard&&) = default;
        Guard& operator=(Guard&&) = default;

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

        Guard(ClientStrand* strand) : _strand(strand) {
            // Hold the lock for as long as the Guard is around. This forces other consumers to
            // queue behind the Guard.
            _strand->_mutex.lock();
            _strand->_isBound.store(true);

            _strand->_setCurrent();
        }

        ~Guard() {
            dismiss();
        }

        void dismiss() noexcept {
            auto strand = std::exchange(_strand, {});
            if (!strand) {
                return;
            }

            strand->_releaseCurrent();
            strand->_isBound.store(false);
            strand->_mutex.unlock();
        }

        Client* get() noexcept {
            return _strand->getClientPointer();
        }

        Client* operator->() noexcept {
            return get();
        }

        Client& operator*() noexcept {
            return *get();
        }

    private:
        boost::intrusive_ptr<ClientStrand> _strand;
    };

    /**
     * A simple wrapping executor to run tasks while a Client is bound.
     */
    class Executor final : public OutOfLineExecutor {
    public:
        Executor(ClientStrand* strand, ExecutorPtr exec)
            : _strand(strand), _exec(std::move(exec)) {}
        void schedule(Task task) override;

    private:
        boost::intrusive_ptr<ClientStrand> _strand;
        ExecutorPtr _exec;
    };

    /**
     * Make a new ClientStrand from a UniqueClient.
     */
    static boost::intrusive_ptr<ClientStrand> make(ServiceContext::UniqueClient client);

    /**
     * Acquire an owning ClientStrand given a client.
     *
     * This will return nullptr if the Client does not belong to a ClientStrand.
     */
    static boost::intrusive_ptr<ClientStrand> get(Client* client);

    ClientStrand(ServiceContext::UniqueClient client)
        : _clientPtr(client.get()),
          _client(std::move(client)),
          _threadName(make_intrusive<ThreadName>(_client->desc())) {}

    /**
     * Get a pointer to the underlying Client.
     */
    Client* getClientPointer() noexcept {
        return _clientPtr;
    }

    /**
     * Set the current Client for this thread and return a RAII guard to release it eventually.
     *
     * If the Client is currently bound, this function will block until the Client is available.
     */
    auto bind() {
        return Guard(this);
    }

    /**
     * Run a Task with the Client bound to the current thread.
     *
     * This function runs the task inline and assumes that the Client is not already bound to the
     * current thread. If the Client is currently bound, this function will block until it is
     * released.
     */
    template <typename Task, typename... Args>
    void run(Task task, Args&&... args) {
        auto guard = bind();

        return task(std::forward<Args>(args)...);
    }

    /**
     * Make a wrapped executor around another.
     */
    ExecutorPtr makeExecutor(ExecutorPtr exec) {
        return std::make_shared<Executor>(this, std::move(exec));
    }

    /**
     * Return if the strand is currently bound to a Client.
     */
    bool isBound() const noexcept {
        return _isBound.load();
    }

private:
    /**
     * Bind the Client to the current thread.
     *
     * This is only valid to call if no other thread has the Client bound.
     */
    void _setCurrent() noexcept;

    /**
     * Release the Client from the current thread.
     *
     * This is valid to call multiple times on the same thread. It is not valid to mix this with
     * Client::releaseCurrent().
     */
    void _releaseCurrent() noexcept;

    Client* const _clientPtr;

    stdx::mutex _mutex;  // NOLINT

    // Once we have stdx::atomic::wait(), we can get rid of the mutex in favor of this variable.
    AtomicWord<bool> _isBound{false};

    ServiceContext::UniqueClient _client;

    boost::intrusive_ptr<ThreadName> _threadName;
    boost::intrusive_ptr<ThreadName> _oldThreadName;
};

inline void ClientStrand::Executor::schedule(Task task) {
    _exec->schedule([task = std::forward<Task>(task), strand = _strand](Status status) mutable {
        strand->run(std::move(task), std::move(status));
    });
}

using ClientStrandPtr = boost::intrusive_ptr<ClientStrand>;

}  // namespace mongo
