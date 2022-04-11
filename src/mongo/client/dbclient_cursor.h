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

#include <stack>

#include "mongo/client/query.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/rpc/message.h"

namespace mongo {

class AScopedConnection;
class DBClientBase;
class AggregateCommandRequest;

/**
 * The internal client's cursor representation for find or agg cursors. The cursor is iterated by
 * the caller using the 'more()' and 'next()' methods. Any necessary getMore requests are
 * constructed and issued internally.
 */
class DBClientCursor {
    DBClientCursor(const DBClientCursor&) = delete;
    DBClientCursor& operator=(const DBClientCursor&) = delete;

public:
    static StatusWith<std::unique_ptr<DBClientCursor>> fromAggregationRequest(
        DBClientBase* client,
        AggregateCommandRequest aggRequest,
        bool secondaryOk,
        bool useExhaust);

    DBClientCursor(DBClientBase* client,
                   const NamespaceStringOrUUID& nsOrUuid,
                   const BSONObj& filter,
                   const Query& querySettings,
                   int limit,
                   int nToSkip,
                   const BSONObj* fieldsToReturn,
                   int queryOptions,
                   int bs,
                   boost::optional<BSONObj> readConcernObj = boost::none);

    DBClientCursor(DBClientBase* client,
                   const NamespaceStringOrUUID& nsOrUuid,
                   long long cursorId,
                   int limit,
                   int options,
                   std::vector<BSONObj> initialBatch = {},
                   boost::optional<Timestamp> operationTime = boost::none,
                   boost::optional<BSONObj> postBatchResumeToken = boost::none);

    DBClientCursor(DBClientBase* client,
                   FindCommandRequest findRequest,
                   const ReadPreferenceSetting& readPref);

    virtual ~DBClientCursor();

    /**
     * If true, safe to call next(). Requests more from server if necessary.
     */
    virtual bool more();

    bool hasMoreToCome() const {
        return _connectionHasPendingReplies;
    }

    /**
     * If true, there is more in our local buffers to be fetched via next(). Returns false when a
     * getMore request back to server would be required. You can use this if you want to exhaust
     * whatever data has been fetched to the client already but then perhaps stop.
     */
    int objsLeftInBatch() const {
        return _putBack.size() + _batch.objs.size() - _batch.pos;
    }
    bool moreInCurrentBatch() {
        return objsLeftInBatch() > 0;
    }

    /**
     * Returns the next object from the cursor.
     *
     * On error at the remote server, you will get back:
     *    {$err: <std::string>
     *
     * If you do not want to handle that yourself, call 'nextSafe()'.
     */
    virtual BSONObj next();

    /**
     * Restores an object previously returned by next() to the cursor.
     */
    void putBack(const BSONObj& o) {
        _putBack.push(o.getOwned());
    }

    /**
     * Similar to 'next()', but throws an AssertionException on error.
     */
    BSONObj nextSafe();

    /**
     * Peek ahead at items buffered for future next() calls. Never requests new data from the
     * server.
     *
     * WARNING: no support for _putBack yet!
     */
    void peek(std::vector<BSONObj>&, int atMost);

    /**
     * Peeks at first element. If no first element exists, returns an empty object.
     */
    BSONObj peekFirst();

    /**
     * peek ahead and see if an error occurred, and get the error if so.
     */
    bool peekError(BSONObj* error = nullptr);

    /**
     * Iterates the rest of the cursor and returns the resulting number if items.
     */
    int itcount() {
        int c = 0;
        while (more()) {
            next();
            c++;
        }
        return c;
    }

    /**
     * Returns true if the cursor is no longer open on the remote node (the remote node has returned
     * a cursor id of zero).
     */
    bool isDead() const {
        return _cursorId == 0;
    }

    bool tailable() const {
        return (_opts & QueryOption_CursorTailable) != 0;
    }

    bool tailableAwaitData() const {
        return tailable() && (_opts & QueryOption_AwaitData);
    }

    /**
     * Changes the cursor's batchSize after construction. Can change after requesting first batch.
     */
    void setBatchSize(int newBatchSize) {
        _batchSize = newBatchSize;
    }

    long long getCursorId() const {
        return _cursorId;
    }

    void attach(AScopedConnection* conn);

    std::string originalHost() const {
        return _originalHost;
    }

    std::string getns() const {
        return _ns.ns();
    }

    const NamespaceString& getNamespaceString() const {
        return _ns;
    }

    /**
     * Performs the initial query, opening the cursor.
     */
    bool init();

    /**
     * Marks this object as dead and sends the KillCursors message to the server.
     *
     * Any errors that result from this are swallowed since this is typically performed as part of
     * cleanup and a failure to kill the cursor should not result in a failure of the operation
     * using the cursor.
     *
     * Killing an already killed or exhausted cursor does nothing, so it is safe to always call this
     * if you want to ensure that a cursor is killed.
     */
    void kill();

    /**
     * Returns true if the connection this cursor is using has pending replies.
     *
     * If true, you should not try to use the connection for any other purpose or return it to a
     * pool.
     *
     * This can happen if an exhaust query was started but not completed.
     */
    bool connectionHasPendingReplies() const {
        return _connectionHasPendingReplies;
    }

    Milliseconds getAwaitDataTimeoutMS() const {
        return _awaitDataTimeout;
    }

    void setAwaitDataTimeoutMS(Milliseconds timeout) {
        // It only makes sense to set awaitData timeout if the cursor is in tailable awaitData mode.
        invariant(tailableAwaitData());
        _awaitDataTimeout = timeout;
    }

    // Only used for tailable awaitData oplog fetching requests.
    void setCurrentTermAndLastCommittedOpTime(
        const boost::optional<long long>& term,
        const boost::optional<repl::OpTime>& lastCommittedOpTime) {
        invariant(tailableAwaitData());
        _term = term;
        _lastKnownCommittedOpTime = lastCommittedOpTime;
    }

    /**
     * Returns the resume token for the latest batch, it set.
     */
    virtual boost::optional<BSONObj> getPostBatchResumeToken() const {
        return _postBatchResumeToken;
    }

    /**
     * Returns the operation time for the latest batch, if set.
     */
    virtual boost::optional<Timestamp> getOperationTime() const {
        return _operationTime;
    }

protected:
    struct Batch {
        // TODO remove constructors after c++17 toolchain upgrade
        Batch() = default;
        Batch(std::vector<BSONObj> initial, size_t initialPos = 0)
            : objs(std::move(initial)), pos(initialPos) {}
        std::vector<BSONObj> objs;
        size_t pos = 0;
    };

    Batch _batch;

private:
    DBClientCursor(DBClientBase* client,
                   const NamespaceStringOrUUID& nsOrUuid,
                   const BSONObj& filter,
                   const Query& querySettings,
                   long long cursorId,
                   int limit,
                   int nToSkip,
                   const BSONObj* fieldsToReturn,
                   int queryOptions,
                   int bs,
                   std::vector<BSONObj> initialBatch,
                   boost::optional<BSONObj> readConcernObj,
                   boost::optional<Timestamp> operationTime,
                   boost::optional<BSONObj> postBatchResumeToken = boost::none);

    void dataReceived(const Message& reply) {
        bool retry;
        std::string lazyHost;
        dataReceived(reply, retry, lazyHost);
    }
    void dataReceived(const Message& reply, bool& retry, std::string& lazyHost);

    /**
     * Parses and returns command replies regardless of which command protocol was used.
     * Does *not* parse replies from non-command OP_QUERY finds.
     */
    BSONObj commandDataReceived(const Message& reply);

    void requestMore();

    void exhaustReceiveMore();

    Message assembleInit();
    Message assembleGetMore();

    /**
     * Constructs the initial find commmand request based on a legacy OP_QUERY-style description of
     * the find operation. Only used if the caller constructed the 'DBClientCursor' with the legacy
     * API.
     */
    Message initFromLegacyRequest();

    DBClientBase* _client;
    std::string _originalHost;
    NamespaceStringOrUUID _nsOrUuid;

    // 'ns' is initially the NamespaceString passed in, or the dbName if doing a find by UUID.
    // After a successful 'find' command, 'ns' is updated to contain the namespace returned by that
    // command.
    NamespaceString _ns;

    long long _cursorId = 0;

    std::stack<BSONObj> _putBack;
    std::string _scopedHost;
    bool _wasError = false;
    bool _connectionHasPendingReplies = false;
    int _lastRequestId = 0;

    int _batchSize;
    int _limit = 0;

    // If the caller describes the find command being executed by this cursor as a
    // 'FindCommandRequest', then that request object and the associated read preference are set
    // here. Otherwise, if the caller uses the legacy OP_QUERY-inspired API, these members are
    // default-initialized but never used.
    boost::optional<FindCommandRequest> _findRequest;
    ReadPreferenceSetting _readPref;

    // These data members are only used if the cursor was constructed using the legacy
    // OP_QUERY-inspired API. If the cursor was constructed using the 'FindCommandRequest'-based
    // API, these are initialized to their default values but never used.
    BSONObj _filter;
    Query _querySettings;
    int _nToSkip = 0;
    const BSONObj* _fieldsToReturn = nullptr;
    boost::optional<BSONObj> _readConcernObj;

    // This has the same meaning as the flags bit vector from the no-longer-supported OP_QUERY wire
    // protocol message. However, it is initialized even if the caller constructed the cursor using
    // the 'FindCommandRequest`-based API.
    //
    // We should eventually stop using the OP_QUERY flags bit vector in server code, since OP_QUERY
    // is no longer supported.
    int _opts;

    Milliseconds _awaitDataTimeout = Milliseconds{0};
    boost::optional<long long> _term;
    boost::optional<repl::OpTime> _lastKnownCommittedOpTime;
    boost::optional<Timestamp> _operationTime;
    boost::optional<BSONObj> _postBatchResumeToken;
};

/** iterate over objects in current batch only - will not cause a network call
 */
class DBClientCursorBatchIterator {
public:
    DBClientCursorBatchIterator(DBClientCursor& c) : _c(c), _n() {}
    bool moreInCurrentBatch() {
        return _c.moreInCurrentBatch();
    }
    BSONObj nextSafe() {
        massert(13383, "BatchIterator empty", moreInCurrentBatch());
        ++_n;
        return _c.nextSafe();
    }
    int n() const {
        return _n;
    }
    // getNamespaceString() will return the NamespaceString returned by the 'find' command.
    const NamespaceString& getNamespaceString() {
        return _c.getNamespaceString();
    }

    long long getCursorId() const {
        return _c.getCursorId();
    }

    boost::optional<BSONObj> getPostBatchResumeToken() const {
        return _c.getPostBatchResumeToken();
    }

private:
    DBClientCursor& _c;
    int _n;
};

}  // namespace mongo
