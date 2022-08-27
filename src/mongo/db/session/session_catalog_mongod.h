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

#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod_transaction_interface.h"

namespace mongo {

class SessionsCollection;

class MongoDSessionCatalog {
    MongoDSessionCatalog(const MongoDSessionCatalog&) = delete;
    MongoDSessionCatalog& operator=(const MongoDSessionCatalog&) = delete;

public:
    class CheckoutTag {};

    /**
     * Retrieves the mongod session transaction table associated with the service or operation
     * context.
     */
    static MongoDSessionCatalog* get(OperationContext* opCtx);
    static MongoDSessionCatalog* get(ServiceContext* service);

    /**
     * Sets the mongod session transaction table associated with the service or operation context.
     */
    static void set(ServiceContext* service, std::unique_ptr<MongoDSessionCatalog> sessionCatalog);

    static const std::string kConfigTxnsPartialIndexName;

    // The max batch size is chosen so that a single batch won't exceed the 16MB BSON object size
    // limit.
    static const int kMaxSessionDeletionBatchSize = 10'000;

    /**
     * Returns the specification for the partial index on config.transactions used to support
     * retryable transactions.
     */
    static BSONObj getConfigTxnPartialIndexSpec();

    explicit MongoDSessionCatalog(std::unique_ptr<MongoDSessionCatalogTransactionInterface> ti);

    /**
     * Invoked when the node enters the primary state. Ensures that the transactions collection is
     * created. Throws on severe exceptions due to which it is not safe to continue the step-up
     * process.
     */
    void onStepUp(OperationContext* opCtx);

    /**
     * Fetches the UUID of the transaction table, or an empty optional if the collection does not
     * exist or has no UUID. Acquires a lock on the collection.
     *
     * Required for rollback via refetch.
     */
    boost::optional<UUID> getTransactionTableUUID(OperationContext* opCtx);

    /**
     * Callback to be invoked in response to insert/update/delete of 'config.transactions' in order
     * to notify the session catalog that the on-disk contents are out of sync with the in-memory
     * state. The 'singleSessionDoc' must contain the _id of the session which was updated.
     */
    void observeDirectWriteToConfigTransactions(OperationContext* opCtx, BSONObj singleSessionDoc);

    /**
     * Callback to be invoked when the contents of 'config.transactions' are out of sync with that
     * in the in-memory catalog, such as when rollback happens or drop of 'config.transactions'.
     */
    void invalidateAllSessions(OperationContext* opCtx);

    /**
     * Locates session entries from the in-memory catalog and in 'config.transactions' which have
     * not been referenced before 'possiblyExpired' and deletes them.
     *
     * Returns the number of sessions, which were reaped from the persisted store on disk.
     */
    int reapSessionsOlderThan(OperationContext* opCtx,
                              SessionsCollection& sessionsCollection,
                              Date_t possiblyExpired);

    /**
     * Deletes the given session ids from config.transactions and config.image_collection.
     */
    int removeSessionsTransactionRecords(OperationContext* opCtx,
                                         const std::vector<LogicalSessionId>& lsidsToRemove);

    /**
     * Functions to check out a session. Returns a scoped object that checks in the session on
     * destruction.
     */
    class Session {
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

    public:
        Session() = default;
        virtual ~Session() = default;

        /**
         * This method allows a checked-out session to be temporarily or permanently checked
         * back in, in order to allow other operations to use it.
         *
         * Applies to Session objects returned by checkOutSession() only.
         *
         * May only be called if the session has actually been checked out previously.
         */
        virtual void checkIn(OperationContext* opCtx,
                             OperationContextSession::CheckInReason reason) = 0;

        /**
         * Applies to Session objects returned by checkOutSession() only.
         *
         * May only be called if the session is not checked out already.
         */
        virtual void checkOut(OperationContext* opCtx) = 0;
    };

    /**
     * Checks out the session specified in the passed operation context and stores it
     * for later access by the command. The session is installed when this method returns
     * and is removed at when the returned Session object goes out of scope.
     */
    std::unique_ptr<Session> checkOutSession(OperationContext* opCtx);

    /**
     * Similar to checkOutSession(), but marks the TransactionParticipant as valid without
     * refreshing from disk and starts a new transaction unconditionally.
     *
     * Returns a scoped Session object that does not support checkIn() or checkOut().
     *
     * NOTE: Only used by the replication oplog application logic on secondaries in order to replay
     * prepared transactions.
     */
    std::unique_ptr<Session> checkOutSessionWithoutRefresh(OperationContext* opCtx);

    /**
     * Similar to checkOutSession(), but marks the TransactionParticipant as valid without
     * loading the retryable write oplog history.  If the last operation was a multi-document
     * transaction, is equivalent to MongoDOperationContextSession.
     *
     * Returns a scoped Session object that does not support checkIn() or checkOut().
     *
     * NOTE: Should only be used when reading the oplog history is not possible.
     */
    std::unique_ptr<Session> checkOutSessionWithoutOplogRead(OperationContext* opCtx);

    /**
     * These are lower-level functions for checking in or out sessions without a scoped Session
     * object (see checkOutSession*() functions above).
     * Used to implement checkIn()/checkOut() in MongoDOperationContextSession.
     */
    void checkInUnscopedSession(OperationContext* opCtx,
                                OperationContextSession::CheckInReason reason);
    void checkOutUnscopedSession(OperationContext* opCtx);

private:
    std::unique_ptr<MongoDSessionCatalogTransactionInterface> _ti;
};

/**
 * Scoped object, which checks out the session specified in the passed operation context and stores
 * it for later access by the command. The session is installed at construction time and is removed
 * at destruction.
 */
class MongoDOperationContextSession : public MongoDSessionCatalog::Session {
    MongoDOperationContextSession(const MongoDOperationContextSession&) = delete;
    MongoDOperationContextSession& operator=(const MongoDOperationContextSession&) = delete;

public:
    MongoDOperationContextSession(OperationContext* opCtx,
                                  MongoDSessionCatalogTransactionInterface* ti);
    ~MongoDOperationContextSession();

    /**
     * This method takes an operation context with a checked-out session and allows it to be
     * temporarily or permanently checked back in, in order to allow other operations to use it.
     *
     * May only be called if the session has actually been checked out previously.
     */
    void checkIn(OperationContext* opCtx, OperationContextSession::CheckInReason reason) override;

    /**
     * May only be called if the session is not checked out already.
     */
    void checkOut(OperationContext* opCtx) override;

private:
    OperationContextSession _operationContextSession;
    MongoDSessionCatalogTransactionInterface* _ti;
};

/**
 * Similar to MongoDOperationContextSession, but marks the TransactionParticipant as valid without
 * refreshing from disk and starts a new transaction unconditionally.
 *
 * NOTE: Only used by the replication oplog application logic on secondaries in order to replay
 * prepared transactions.
 */
class MongoDOperationContextSessionWithoutRefresh : public MongoDSessionCatalog::Session {
    MongoDOperationContextSessionWithoutRefresh(
        const MongoDOperationContextSessionWithoutRefresh&) = delete;
    MongoDOperationContextSessionWithoutRefresh& operator=(
        const MongoDOperationContextSessionWithoutRefresh&) = delete;

public:
    MongoDOperationContextSessionWithoutRefresh(OperationContext* opCtx,
                                                MongoDSessionCatalog::CheckoutTag tag);
    ~MongoDOperationContextSessionWithoutRefresh();

    void checkIn(OperationContext* opCtx, OperationContextSession::CheckInReason reason) override {
        MONGO_UNREACHABLE;
    }

    void checkOut(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

private:
    OperationContextSession _operationContextSession;
    OperationContext* const _opCtx;
};

/**
 * Similar to MongoDOperationContextSession, but marks the TransactionParticipant as valid without
 * loading the retryable write oplog history.  If the last operation was a multi-document
 * transaction, is equivalent to MongoDOperationContextSession.
 *
 * NOTE: Should only be used when reading the oplog history is not possible.
 */
class MongoDOperationContextSessionWithoutOplogRead : public MongoDSessionCatalog::Session {
    MongoDOperationContextSessionWithoutOplogRead(
        const MongoDOperationContextSessionWithoutOplogRead&) = delete;
    MongoDOperationContextSessionWithoutOplogRead& operator=(
        const MongoDOperationContextSessionWithoutOplogRead&) = delete;

public:
    MongoDOperationContextSessionWithoutOplogRead(OperationContext* opCtx,
                                                  MongoDSessionCatalogTransactionInterface* ti);
    ~MongoDOperationContextSessionWithoutOplogRead();

    void checkIn(OperationContext* opCtx, OperationContextSession::CheckInReason reason) override {
        MONGO_UNREACHABLE;
    }

    void checkOut(OperationContext* opCtx) override {
        MONGO_UNREACHABLE;
    }

private:
    OperationContextSession _operationContextSession;
    OperationContext* const _opCtx;
};

}  // namespace mongo
