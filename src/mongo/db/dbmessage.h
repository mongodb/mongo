// dbmessage.h

/**
*    Copyright (C) 2008 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/base/static_assert.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/client/constants.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/server_options.h"
#include "mongo/util/net/message.h"

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
/* see http://dochub.mongodb.org/core/mongowireprotocol
*/
struct Layout {
    MsgData::Layout msgdata;
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

    void setCursorId(int64_t value) {
        storage().write(tagLittleEndian(value), offsetof(Layout, cursorId));
    }

    void setStartingFrom(int32_t value) {
        storage().write(tagLittleEndian(value), offsetof(Layout, startingFrom));
    }

    void setNReturned(int32_t value) {
        storage().write(tagLittleEndian(value), offsetof(Layout, nReturned));
    }

    int32_t getResultFlags() {
        return DataView(msgdata().data()).read<LittleEndian<int32_t>>();
    }

    void setResultFlags(int32_t value) {
        DataView(msgdata().data()).write(tagLittleEndian(value));
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

/* For the database/server protocol, these objects and functions encapsulate
   the various messages transmitted over the connection.

   See http://dochub.mongodb.org/core/mongowireprotocol
*/
class DbMessage {
    // Assume sizeof(int) == 4 bytes
    MONGO_STATIC_ASSERT(sizeof(int) == 4);

public:
    // Note: DbMessage constructor reads the first 4 bytes and stores it in reserved
    DbMessage(const Message& msg);

    // Indicates whether this message is expected to have a ns
    bool messageShouldHaveNs() const {
        return (_msg.operation() >= dbUpdate) & (_msg.operation() <= dbDelete);
    }

    /** the 32 bit field before the ns
     * track all bit usage here as its cross op
     * 0: InsertOption_ContinueOnError
     * 1: fromWriteback
     */
    int reservedField() const {
        return _reserved;
    }

    const char* getns() const;
    int getQueryNToReturn() const;

    int pullInt();
    long long pullInt64();
    const char* getArray(size_t count) const;

    /* for insert and update msgs */
    bool moreJSObjs() const {
        return _nextjsobj != 0 && _nextjsobj != _theEnd;
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

/** the query field 'options' can have these bits set: */
enum QueryOptions {
    /** Tailable means cursor is not closed when the last data is retrieved.  rather, the cursor
     * marks the final object's position.  you can resume using the cursor later, from where it was
       located, if more data were received.  Set on dbQuery and dbGetMore.

       like any "latent cursor", the cursor may become invalid at some point -- for example if that
       final object it references were deleted.  Thus, you should be prepared to requery if you get
       back ResultFlag_CursorNotFound.
    */
    QueryOption_CursorTailable = 1 << 1,

    /** allow query of replica slave.  normally these return an error except for namespace "local".
    */
    QueryOption_SlaveOk = 1 << 2,

    // findingStart mode is used to find the first operation of interest when
    // we are scanning through a repl log.  For efficiency in the common case,
    // where the first operation of interest is closer to the tail than the head,
    // we start from the tail of the log and work backwards until we find the
    // first operation of interest.  Then we scan forward from that first operation,
    // actually returning results to the client.  During the findingStart phase,
    // we release the db mutex occasionally to avoid blocking the db process for
    // an extended period of time.
    QueryOption_OplogReplay = 1 << 3,

    /** The server normally times out idle cursors after an inactivity period to prevent excess
     * memory uses
        Set this option to prevent that.
    */
    QueryOption_NoCursorTimeout = 1 << 4,

    /** Use with QueryOption_CursorTailable.  If we are at the end of the data, block for a while
     * rather than returning no data. After a timeout period, we do return as normal.
    */
    QueryOption_AwaitData = 1 << 5,

    /** Stream the data down full blast in multiple "more" packages, on the assumption that the
     * client will fully read all data queried.  Faster when you are pulling a lot of data and know
     * you want to pull it all down.  Note: it is not allowed to not read all the data unless you
     * close the connection.

        Use the query( stdx::function<void(const BSONObj&)> f, ... ) version of the connection's
        query()
        method, and it will take care of all the details for you.
    */
    QueryOption_Exhaust = 1 << 6,

    /** When sharded, this means its ok to return partial results
        Usually we will fail a query if all required shards aren't up
        If this is set, it'll be a partial result set
     */
    QueryOption_PartialResults = 1 << 7,

    // DBClientCursor reserves flag 1 << 30 to force the use of OP_QUERY.

    QueryOption_AllSupported = QueryOption_CursorTailable | QueryOption_SlaveOk |
        QueryOption_OplogReplay | QueryOption_NoCursorTimeout | QueryOption_AwaitData |
        QueryOption_Exhaust | QueryOption_PartialResults,

    QueryOption_AllSupportedForSharding = QueryOption_CursorTailable | QueryOption_SlaveOk |
        QueryOption_OplogReplay | QueryOption_NoCursorTimeout | QueryOption_AwaitData |
        QueryOption_PartialResults,
};

/* a request to run a query, received from the database */
class QueryMessage {
public:
    const char* ns;
    int ntoskip;
    int ntoreturn;
    int queryOptions;
    BSONObj query;
    BSONObj fields;

    /**
     * parses the message into the above fields
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
     * A non-muting constructor from the whole message.
     */
    explicit QueryMessage(const Message& message) {
        DbMessage dbm(message);
        *this = QueryMessage(dbm);
    }
};

enum InsertOptions {
    /** With muli-insert keep processing inserts if one fails */
    InsertOption_ContinueOnError = 1 << 0
};

/**
 * Builds a legacy OP_INSERT message.
 */
Message makeInsertMessage(StringData ns, const BSONObj* objs, size_t count, int flags = 0);
inline Message makeInsertMessage(StringData ns, const BSONObj& obj, int flags = 0) {
    return makeInsertMessage(ns, &obj, 1, flags);
}

enum UpdateOptions {
    /** Upsert - that is, insert the item if no matching item is found. */
    UpdateOption_Upsert = 1 << 0,

    /** Update multiple documents (if multiple documents match query expression).
       (Default is update a single document and stop.) */
    UpdateOption_Multi = 1 << 1,

    /** flag from mongo saying this update went everywhere */
    UpdateOption_Broadcast = 1 << 2
};

/**
 * Builds a legacy OP_UPDATE message.
 */
Message makeUpdateMessage(StringData ns, BSONObj query, BSONObj update, int flags = 0);

enum RemoveOptions {
    /** only delete one option */
    RemoveOption_JustOne = 1 << 0,

    /** flag from mongo saying this update went everywhere */
    RemoveOption_Broadcast = 1 << 1
};

/**
 * Builds a legacy OP_REMOVE message.
 */
Message makeRemoveMessage(StringData ns, BSONObj query, int flags = 0);

/**
 * Builds a legacy OP_KILLCURSORS message.
 */
Message makeKillCursorsMessage(long long cursorId);

/**
 * Builds a legacy OP_GETMORE message.
 */
Message makeGetMoreMessage(StringData ns, long long cursorId, int nToReturn, int flags = 0);

/**
 * A response to a DbMessage.
 *
 * Order of fields makes DbResponse{funcReturningMessage()} valid.
 */
struct DbResponse {
    Message response;       // If empty, nothing will be returned to the client.
    std::string exhaustNS;  // Namespace of cursor if exhaust mode, else "".
};

/**
 * Prepares query replies to legacy finds (opReply to dbQuery) in place. This is also used for
 * command responses that don't use the new dbCommand protocol.
 */
class OpQueryReplyBuilder {
    MONGO_DISALLOW_COPYING(OpQueryReplyBuilder);

public:
    OpQueryReplyBuilder();

    /**
     * Returns the BufBuilder that should be used for placing result objects. It will be positioned
     * where the first (or next) object should go.
     *
     * You must finish the BSONObjBuilder that uses this (by destruction or calling doneFast())
     * before calling any more methods on this object.
     */
    BufBuilder& bufBuilderForResults() {
        return _buffer;
    }

    /**
     * Finishes the reply and returns the message buffer.
     */
    Message toQueryReply(int queryResultFlags,
                         int nReturned,
                         int startingFrom = 0,
                         long long cursorId = 0);

    /**
     * Similar to toQueryReply() but used for replying to a command.
     */
    Message toCommandReply() {
        return toQueryReply(0, 1);
    }

private:
    BufBuilder _buffer;
};

/**
 * Helper to build a DbResponse from a buffer containing an OP_QUERY response.
 */
DbResponse replyToQuery(int queryResultFlags,
                        const void* data,
                        int size,
                        int nReturned,
                        int startingFrom = 0,
                        long long cursorId = 0);


/**
 * Helper to build a DbRespose for OP_QUERY with a single reply object.
 */
inline DbResponse replyToQuery(const BSONObj& obj, int queryResultFlags = 0) {
    return replyToQuery(queryResultFlags, obj.objdata(), obj.objsize(), /*nReturned*/ 1);
}
}  // namespace mongo
