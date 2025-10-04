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

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/encoded_value_storage.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/constants.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/message.h"

#include <cstdint>
#include <cstdio>

#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;

/* db response format

   Query or GetMore: // see struct QueryResult
      int resultFlags;
      int64 cursorID;
      int startingFrom;
      int nReturned;
      list of marshalled JSObjects;
*/

/* db request message format

   unsigned opid;         // arbitary; will be echoed back
   byte operation;
   int options;

   then for:

   dbInsert:
      std::string collection;
      a series of JSObjects
   dbDelete:
      std::string collection;
      int flags=0; // 1=DeleteSingle
      JSObject query;
   dbUpdate:
      std::string collection;
      int flags; // 1=upsert
      JSObject query;
      JSObject objectToUpdate;
        objectToUpdate may include { $inc: <field> } or { $set: ... }, see struct Mod.
   dbQuery:
      std::string collection;
      int nToSkip;
      int nToReturn; // how many you want back as the beginning of the cursor data (0=no limit)
                     // greater than zero is simply a hint on how many objects to send back per
                     // "cursor batch".
                     // a negative number indicates a hard limit.
      JSObject query;
      [JSObject fieldsToReturn]
   dbGetMore:
      std::string collection; // redundant, might use for security.
      int nToReturn;
      int64 cursorID;
   dbKillCursors=2007:
      int n;
      int64 cursorIDs[n];

   Note that on Update, there is only one object, which is different
   from insert where you can pass a list of objects to insert in the db.
   Note that the update field layout is very similar layout to Query.
*/

namespace QueryResult {
#pragma pack(1)
/**
 * See http://dochub.mongodb.org/core/mongowireprotocol.
 */
struct Layout {
    MsgData::Layout msgdata;
    int32_t resultFlags;
    int64_t cursorId;
    int32_t startingFrom;
    int32_t nReturned;
};
#pragma pack()

class ConstView {
public:
    ConstView(const char* storage) : _storage(storage) {}

    const char* view2ptr() const {
        return storage().view();
    }

    MsgData::ConstView msgdata() const {
        return storage().view(offsetof(Layout, msgdata));
    }

    int32_t getResultFlags() const {
        return storage().read<LittleEndian<int32_t>>(offsetof(Layout, resultFlags));
    }

    int64_t getCursorId() const {
        return storage().read<LittleEndian<int64_t>>(offsetof(Layout, cursorId));
    }

    int32_t getStartingFrom() const {
        return storage().read<LittleEndian<int32_t>>(offsetof(Layout, startingFrom));
    }

    int32_t getNReturned() const {
        return storage().read<LittleEndian<int32_t>>(offsetof(Layout, nReturned));
    }

    const char* data() const {
        return storage().view(sizeof(Layout));
    }

    int32_t dataLen() const {
        return msgdata().getLen() - sizeof(Layout);
    }

protected:
    const ConstDataView& storage() const {
        return _storage;
    }

private:
    ConstDataView _storage;
};

class View : public ConstView {
public:
    View(char* data) : ConstView(data) {}

    using ConstView::view2ptr;
    char* view2ptr() {
        return storage().view();
    }

    using ConstView::msgdata;
    MsgData::View msgdata() {
        return storage().view(offsetof(Layout, msgdata));
    }

    void setResultFlags(int32_t value) {
        storage().write(tagLittleEndian(value), offsetof(Layout, resultFlags));
    }

    void setCursorId(int64_t value) {
        storage().write(tagLittleEndian(value), offsetof(Layout, cursorId));
    }

    void setStartingFrom(int32_t value) {
        storage().write(tagLittleEndian(value), offsetof(Layout, startingFrom));
    }

    void setNReturned(int32_t value) {
        storage().write(tagLittleEndian(value), offsetof(Layout, nReturned));
    }

    void setResultFlagsToOk() {
        setResultFlags(ResultFlag_AwaitCapable);
    }

    void initializeResultFlags() {
        setResultFlags(0);
    }

private:
    DataView storage() const {
        return const_cast<char*>(ConstView::view2ptr());
    }
};

class Value : public EncodedValueStorage<Layout, ConstView, View> {
public:
    Value() {
        MONGO_STATIC_ASSERT(sizeof(Value) == sizeof(Layout));
    }

    Value(ZeroInitTag_t zit) : EncodedValueStorage<Layout, ConstView, View>(zit) {}
};

}  // namespace QueryResult

/**
 * For the database/server protocol, these objects and functions encapsulate the various messages
 * transmitted over the connection.
 *
 * See http://dochub.mongodb.org/core/mongowireprotocol.
 */
class DbMessage {
    // Assume sizeof(int) == 4 bytes
    MONGO_STATIC_ASSERT(sizeof(int) == 4);

public:
    /**
     * Note: DbMessage constructor reads the first 4 bytes and stores it in reserved
     */
    DbMessage(const Message& msg);

    /**
     * Indicates whether this message is expected to have a ns.
     */
    bool messageShouldHaveNs() const {
        return static_cast<int>(_msg.operation() >= dbUpdate) & (_msg.operation() <= dbDelete);
    }

    /**
     * Returns the 32 bit field before the ns.
     * track all bit usage here as its cross op
     * 0: InsertOption_ContinueOnError
     * 1: fromWriteback
     */
    int reservedField() const {
        return _reserved;
    }

    const char* getns() const;

    int pullInt();
    long long pullInt64();
    const char* getArray(size_t count) const;

    /**
     * Used by insert and update msgs
     */
    bool moreJSObjs() const {
        return _nextjsobj != nullptr && _nextjsobj != _theEnd;
    }

    BSONObj nextJsObj();

    const Message& msg() const {
        return _msg;
    }

    const char* markGet() const {
        return _nextjsobj;
    }

    void markSet() {
        _mark = _nextjsobj;
    }

    void markReset(const char* toMark);

private:
    // Check if we have enough data to read
    template <typename T>
    void checkRead(const char* start, size_t count = 0) const;

    // Read some type without advancing our position
    template <typename T>
    T read() const;

    // Read some type, and advance our position
    template <typename T>
    T readAndAdvance();

    const Message& _msg;
    int _reserved;  // flags or zero depending on packet, starts the packet

    const char* _nsStart;    // start of namespace string, +4 from message start
    const char* _nextjsobj;  // current position reading packet
    const char* _theEnd;     // end of packet

    const char* _mark;

    unsigned int _nsLen;
};

/**
 * The query field 'options' can have these bits set:
 */
enum QueryOptions {
    /**
     * Tailable means cursor is not closed when the last data is retrieved. Rather, the cursor
     * marks the final object's position. You can resume using the cursor later, from where it was
     * located, if more data were received. Set on dbQuery and dbGetMore.
     *
     *  like any "latent cursor", the cursor may become invalid at some point -- for example if that
     *  final object it references were deleted.
     */
    QueryOption_CursorTailable = 1 << 1,

    /**
     * Allow query of replica secondary. Normally these return an error except for namespace
     * "local".
     */
    QueryOption_SecondaryOk = 1 << 2,

    /**
     * In previous versions of the server, clients were required to set this option in order to
     * enable an optimized oplog scan. As of 4.4, the server will apply the optimization for
     * eligible queries regardless of whether this flag is set.
     *
     * This bit is reserved for compatibility with old clients, but it should not be set by modern
     * clients.
     *
     * New server code should not use this flag.
     */
    QueryOption_OplogReplay_DEPRECATED = 1 << 3,

    /**
     * The server normally times out idle cursors after an inactivity period to prevent excess
     * memory uses. Set this option to prevent that.
     */
    QueryOption_NoCursorTimeout = 1 << 4,

    /**
     * Use with QueryOption_CursorTailable.  If we are at the end of the data, block for a while
     * rather than returning no data. After a timeout period, we do return as normal.
     */
    QueryOption_AwaitData = 1 << 5,

    /**
     * Stream the data down full blast in multiple "more" packages, on the assumption that the
     * client will fully read all data queried.  Faster when you are pulling a lot of data and know
     * you want to pull it all down.  Note: it is not allowed to not read all the data unless you
     * close the connection.
     *
     * Use the query( std::function<void(const BSONObj&)> f, ... ) version of the connection's
     * query() method, and it will take care of all the details for you.
     */
    QueryOption_Exhaust = 1 << 6,

    /**
     * When sharded, this means its ok to return partial results. Usually we will fail a query if
     * all required shards aren't up. If this is set, it'll be a partial result set.
     */
    QueryOption_PartialResults = 1 << 7,

    // DBClientCursor reserves flag 1 << 30 to force the use of OP_QUERY.

    QueryOption_AllSupported = QueryOption_CursorTailable | QueryOption_SecondaryOk |
        QueryOption_NoCursorTimeout | QueryOption_AwaitData | QueryOption_Exhaust |
        QueryOption_PartialResults,
};

/**
 * A request to run a query, received from the database.
 */
class QueryMessage {
public:
    const char* ns;
    int ntoskip;
    int ntoreturn;
    int queryOptions;
    BSONObj query;
    BSONObj fields;

    /**
     * Parses the message into the above fields.
     * Warning: constructor mutates DbMessage.
     */
    explicit QueryMessage(DbMessage& d) {
        ns = d.getns();
        ntoskip = d.pullInt();
        ntoreturn = d.pullInt();
        query = d.nextJsObj();
        if (d.moreJSObjs()) {
            fields = d.nextJsObj();
        }
        queryOptions = DataView(d.msg().header().data()).read<LittleEndian<int32_t>>();
    }

    /**
     * A non-mutating constructor from the whole message.
     */
    explicit QueryMessage(const Message& message) {
        DbMessage dbm(message);
        *this = QueryMessage(dbm);
    }
};

template <typename Func>
Message makeMessage(NetworkOp op, Func&& bodyBuilder) {
    BufBuilder b;
    b.skip(sizeof(MSGHEADER::Layout));

    bodyBuilder(b);

    const int size = b.len();
    auto out = Message(b.release());
    out.header().setOperation(op);
    out.header().setLen(size);
    return out;
}

enum InsertOptions {
    /**
     * With muli-insert keep processing inserts if one fails.
     */
    InsertOption_ContinueOnError = 1 << 0
};

/**
 * Builds a legacy OP_INSERT message.
 *
 * The OP_INSERT command is no longer supported, so new callers of this function should not be
 * added! This is currently retained for the limited purpose of unit testing.
 */
Message makeUnsupportedOpInsertMessage(StringData ns,
                                       const BSONObj* objs,
                                       size_t count,
                                       int flags = 0);

/**
 * A response to a DbMessage.
 *
 * Order of fields makes DbResponse{funcReturningMessage()} valid.
 */
struct DbResponse {
    // If empty, nothing will be returned to the client.
    Message response;

    // For exhaust commands, indicates whether the command should be run again.
    bool shouldRunAgainForExhaust = false;

    // The next invocation for an exhaust command. If this is boost::none, the previous invocation
    // should be reused for the next invocation.
    boost::optional<BSONObj> nextInvocation;
};

/**
 * Helper to build an error DbResponse for OP_QUERY and OP_GET_MORE.
 */
DbResponse makeErrorResponseToUnsupportedOpQuery(StringData errorMsg);
}  // namespace mongo
