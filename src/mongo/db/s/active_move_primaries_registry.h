/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/s/request_types/move_primary_gen.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class ScopedMovePrimary;

/**
 * Thread-safe object that keeps track of active movePrimary commands running on a node and limits
 * them to only one per shard. There is only one instance of this object per shard.
 */

class ActiveMovePrimariesRegistry {
    ActiveMovePrimariesRegistry(const ActiveMovePrimariesRegistry&) = delete;
    ActiveMovePrimariesRegistry& operator=(const ActiveMovePrimariesRegistry&) = delete;

public:
    ActiveMovePrimariesRegistry();
    ~ActiveMovePrimariesRegistry();

    static ActiveMovePrimariesRegistry& get(ServiceContext* service);
    static ActiveMovePrimariesRegistry& get(OperationContext* opCtx);

    /**
     * If there are no movePrimary operations running on this shard, registers an active
     * movePrimary operation with the specified arguments. Returns a ScopedMovePrimary, which must
     * be signaled by the caller before it goes out of scope.
     *
     * If there is an active movePrimary operation already running on this shard and it has the
     * exact same arguments, returns a ScopedMovePrimary, which can be used to join the already
     * running movePrimary command.
     *
     * Otherwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedMovePrimary> registerMovePrimary(const ShardMovePrimary& requestArgs);

    /**
     * If a movePrimary command has been previously registered through a call to
     * registerMovePrimary, returns that namespace. Otherwise returns boost::none.
     */
    boost::optional<NamespaceString> getActiveMovePrimaryNss();

private:
    friend class ScopedMovePrimary;

    // Describes the state of a current active movePrimary operation.
    struct ActiveMovePrimaryState {
        ActiveMovePrimaryState(ShardMovePrimary inRequestArgs)
            : requestArgs(std::move(inRequestArgs)),
              notification(std::make_shared<Notification<Status>>()) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus() const;

        // Exact arguments of the currently active operation
        ShardMovePrimary requestArgs;

        // Notification event, which will be signaled when the currently active operation completes
        std::shared_ptr<Notification<Status>> notification;
    };

    /**
     * Unregisters a previously registered namespace with an ongoing movePrimary operation. Must
     * only be called if a previous call to registerMovePrimary has succeeded.
     */
    void _clearMovePrimary();

    // Protects the state below
    stdx::mutex _mutex;

    // If there is an active movePrimary operation going on, this field contains the request that
    // initiated it.
    boost::optional<ActiveMovePrimaryState> _activeMovePrimaryState;
};

/**
 * An object of this class is returned from the registerMovePrimary call of the current active
 * movePrimaries registry. It can exist in two modes - 'execute' and 'join.' See the comments for
 * registerMovePrimary for more details.
 */
class ScopedMovePrimary {
    ScopedMovePrimary(const ScopedMovePrimary&) = delete;
    ScopedMovePrimary& operator=(const ScopedMovePrimary&) = delete;

public:
    ScopedMovePrimary(ActiveMovePrimariesRegistry* registry,
                      bool shouldExecute,
                      std::shared_ptr<Notification<Status>> completionNotification);
    ~ScopedMovePrimary();

    ScopedMovePrimary(ScopedMovePrimary&&);
    ScopedMovePrimary& operator=(ScopedMovePrimary&&);

    /**
     * Returns true if the registerMovePrimary object is in the 'execute' mode. This means that
     * the registerMovePrimary object holder is next in line to execute the movePrimary command.
     * The holder must execute the command and call signalComplete with a status.
     */
    bool mustExecute() const {
        return _shouldExecute;
    }

    /**
     * Must only be called if the object was in the 'execute' mode when the movePrimary command
     * was invoked (the command immediately executed). Signals any callers that might be blocked in
     * waitForCompletion.
     */
    void signalComplete(Status status);

    /**
     * Must only be called if the object is in the 'join' mode. Blocks until the main executor of
     * the movePrimary command calls signalComplete.
     */
    Status waitForCompletion(OperationContext* opCtx);

private:
    // Registry from which to unregister the movePrimary. Not owned.
    ActiveMovePrimariesRegistry* _registry;

    /* Whether the holder is the first in line to call the movePrimary command (in which case the
     * destructor must unregister) or the caller is joining on an already-running movePrimary
     * operation (in which case the caller must block and wait for completion).
     */
    bool _shouldExecute;

    // This is the future, which will be signaled at the end of a movePrimary command.
    std::shared_ptr<Notification<Status>> _completionNotification;
};
}
