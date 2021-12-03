/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <list>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"

namespace mongo {

/**
 * Observes CRUD operations on interested collections that have an ongoing
 * unique index converstion.
 */
class CollModWriteOpsTracker final {
    CollModWriteOpsTracker(const CollModWriteOpsTracker&) = delete;
    CollModWriteOpsTracker& operator=(const CollModWriteOpsTracker&) = delete;

public:
    // BSONObj is owned.
    using Docs = std::vector<BSONObj>;

    /**
     * Retrieves the lock manager instance attached to this ServiceContext.
     */
    static CollModWriteOpsTracker* get(ServiceContext* service);

    /**
     * Default constructors are meant for unit tests only. The write ops tracker should generally be
     * accessed as a decorator on the ServiceContext.
     */
    CollModWriteOpsTracker() = default;
    ~CollModWriteOpsTracker() = default;

    /**
     * Records CRUD operation event on a namespace.
     */
    void onDocumentChanged(const UUID& uuid, const BSONObj& doc);

    /**
     * Starts tracking write ops on a namespace.
     */
    class Token;
    std::unique_ptr<Token> startTracking(const UUID& uuid);

    /**
     * Stops tracking write ops on a namespace.
     * Unregisters tracking token and returns tracked ops/docs.
     */
    std::unique_ptr<Docs> stopTracking(std::unique_ptr<Token> token);

    /**
     * Called to clean up tracking token when observed docs are not released through
     * stopTracking().
     */
    void onTokenDestroyed(Token* token);

private:
    // Mutex to protect internal state
    Mutex _mutex = MONGO_MAKE_LATCH("CollModWriteOpsTracker::_mutex");

    // List of WriteOps instances to notify.
    std::list<Token*> _tokens;  // not owned
};

/**
 * Opaque token for observing write ops observed on a namespace registered with this tracker.
 * Supports internal implementation for CollModWriteOpsTracker.
 * Not to be accessed directly by callers outside CollModWriteOpsTracker.
 */
class CollModWriteOpsTracker::Token {
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;

public:
    Token(CollModWriteOpsTracker* tracker, const UUID& uuid);
    ~Token();

private:
    friend class CollModWriteOpsTracker;
    CollModWriteOpsTracker* _tracker;
    UUID _uuid;
    std::unique_ptr<CollModWriteOpsTracker::Docs> _docs;
};

}  // namespace mongo
