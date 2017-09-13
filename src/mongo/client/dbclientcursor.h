// file dbclientcursor.h

/*    Copyright 2009 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <stack>

#include "mongo/base/disallow_copying.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/net/message.h"

namespace mongo {

class AScopedConnection;

/** Queries return a cursor object */
class DBClientCursor {
    MONGO_DISALLOW_COPYING(DBClientCursor);

public:
    /** If true, safe to call next().  Requests more from server if necessary. */
    virtual bool more();

    /** If true, there is more in our local buffers to be fetched via next(). Returns
        false when a getMore request back to server would be required.  You can use this
        if you want to exhaust whatever data has been fetched to the client already but
        then perhaps stop.
    */
    int objsLeftInBatch() const {
        return _putBack.size() + batch.objs.size() - batch.pos;
    }
    bool moreInCurrentBatch() {
        return objsLeftInBatch() > 0;
    }

    /** next
       @return next object in the result cursor.
       on an error at the remote server, you will get back:
         { $err: <std::string> }
       if you do not want to handle that yourself, call nextSafe().
    */
    virtual BSONObj next();

    /**
        restore an object previously returned by next() to the cursor
     */
    void putBack(const BSONObj& o) {
        _putBack.push(o.getOwned());
    }

    /** throws AssertionException if get back { $err : ... } */
    BSONObj nextSafe();

    /** peek ahead at items buffered for future next() calls.
        never requests new data from the server.  so peek only effective
        with what is already buffered.
        WARNING: no support for _putBack yet!
    */
    void peek(std::vector<BSONObj>&, int atMost);

    // Peeks at first element, if exists
    BSONObj peekFirst();

    /**
     * peek ahead and see if an error occurred, and get the error if so.
     */
    bool peekError(BSONObj* error = NULL);

    /**
       iterate the rest of the cursor and return the number if items
     */
    int itcount() {
        int c = 0;
        while (more()) {
            next();
            c++;
        }
        return c;
    }

    /** cursor no longer valid -- use with tailable cursors.
       note you should only rely on this once more() returns false;
       'dead' may be preset yet some data still queued and locally
       available from the dbclientcursor.
    */
    bool isDead() const {
        return cursorId == 0;
    }

    bool tailable() const {
        return (opts & QueryOption_CursorTailable) != 0;
    }

    /** see ResultFlagType (constants.h) for flag values
        mostly these flags are for internal purposes -
        ResultFlag_ErrSet is the possible exception to that
    */
    bool hasResultFlag(int flag) {
        return (resultFlags & flag) != 0;
    }

    /// Change batchSize after construction. Can change after requesting first batch.
    void setBatchSize(int newBatchSize) {
        batchSize = newBatchSize;
    }


    /**
     * Fold this in with queryOptions to force the use of legacy query operations.
     * This flag is never sent over the wire and is only used locally.
     */
    enum { QueryOptionLocal_forceOpQuery = 1 << 30 };

    DBClientCursor(DBClientBase* client,
                   const std::string& ns,
                   const BSONObj& query,
                   int nToReturn,
                   int nToSkip,
                   const BSONObj* fieldsToReturn,
                   int queryOptions,
                   int bs);

    DBClientCursor(DBClientBase* client,
                   const std::string& ns,
                   long long cursorId,
                   int nToReturn,
                   int options);

    virtual ~DBClientCursor();

    long long getCursorId() const {
        return cursorId;
    }

    /** by default we "own" the cursor and will send the server a KillCursor
        message when ~DBClientCursor() is called. This function overrides that.
    */
    void decouple() {
        _ownCursor = false;
    }

    void attach(AScopedConnection* conn);

    std::string originalHost() const {
        return _originalHost;
    }

    std::string getns() const {
        return ns.ns();
    }

    /**
     * actually does the query
     */
    bool init();

    void initLazy(bool isRetry = false);
    bool initLazyFinish(bool& retry);

    /**
     * For exhaust. Used in DBClientConnection.
     */
    void exhaustReceiveMore();

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
     * This can happen if either initLazy() was called without initLazyFinish() or an exhaust query
     * was started but not completed.
     */
    bool connectionHasPendingReplies() const {
        return _connectionHasPendingReplies;
    }

private:
    DBClientCursor(DBClientBase* client,
                   const std::string& ns,
                   const BSONObj& query,
                   long long cursorId,
                   int nToReturn,
                   int nToSkip,
                   const BSONObj* fieldsToReturn,
                   int queryOptions,
                   int bs);

    int nextBatchSize();

    struct Batch {
        std::vector<BSONObj> objs;
        size_t pos = 0;
    };

    Batch batch;
    DBClientBase* _client;
    std::string _originalHost;
    NamespaceString ns;
    const bool _isCommand;
    BSONObj query;
    int nToReturn;
    bool haveLimit;
    int nToSkip;
    const BSONObj* fieldsToReturn;
    int opts;
    int batchSize;
    std::stack<BSONObj> _putBack;
    int resultFlags;
    long long cursorId;
    bool _ownCursor;  // see decouple()
    std::string _scopedHost;
    std::string _lazyHost;
    bool wasError;
    BSONVersion _enabledBSONVersion;
    bool _useFindCommand = true;
    bool _connectionHasPendingReplies = false;
    int _lastRequestId = 0;

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

    // init pieces
    Message _assembleInit();
    Message _assembleGetMore();
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

private:
    DBClientCursor& _c;
    int _n;
};

}  // namespace mongo
