// dbmessage.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/db/dbmessage.h"

#include "mongo/platform/strnlen.h"
#include "mongo/rpc/object_check.h"

namespace mongo {

DbMessage::DbMessage(const Message& msg) : _msg(msg), _nsStart(NULL), _mark(NULL), _nsLen(0) {
    // for received messages, Message has only one buffer
    _theEnd = _msg.singleData().data() + _msg.singleData().dataLen();
    _nextjsobj = _msg.singleData().data();

    _reserved = readAndAdvance<int>();

    // Read packet for NS
    if (messageShouldHaveNs()) {
        // Limit = buffer size of message -
        //        (first int4 in message which is either flags or a zero constant)
        size_t limit = _msg.singleData().dataLen() - sizeof(int);

        _nsStart = _nextjsobj;
        _nsLen = strnlen(_nsStart, limit);

        // Validate there is room for a null byte in the buffer
        // Strings can be zero length
        uassert(18633, "Failed to parse ns string", _nsLen < limit);

        _nextjsobj += _nsLen + 1;  // skip namespace + null
    }
}

const char* DbMessage::getns() const {
    verify(messageShouldHaveNs());
    return _nsStart;
}

int DbMessage::getQueryNToReturn() const {
    verify(messageShouldHaveNs());
    const char* p = _nsStart + _nsLen + 1;
    checkRead<int>(p, 2);

    return ConstDataView(p).read<LittleEndian<int32_t>>(sizeof(int32_t));
}

int DbMessage::pullInt() {
    return readAndAdvance<int32_t>();
}

long long DbMessage::pullInt64() {
    return readAndAdvance<int64_t>();
}

const char* DbMessage::getArray(size_t count) const {
    checkRead<long long>(_nextjsobj, count);
    return _nextjsobj;
}

BSONObj DbMessage::nextJsObj() {
    uassert(ErrorCodes::InvalidBSON,
            "Client Error: Remaining data too small for BSON object",
            _nextjsobj != NULL && _theEnd - _nextjsobj >= 5);

    if (serverGlobalParams.objcheck) {
        Status status = validateBSON(
            _nextjsobj, _theEnd - _nextjsobj, Validator<BSONObj>::enabledBSONVersion());
        uassert(ErrorCodes::InvalidBSON,
                str::stream() << "Client Error: bad object in message: " << status.reason(),
                status.isOK());
    }

    BSONObj js(_nextjsobj);
    verify(js.objsize() >= 5);
    verify(js.objsize() <= (_theEnd - _nextjsobj));

    _nextjsobj += js.objsize();
    if (_nextjsobj >= _theEnd)
        _nextjsobj = NULL;
    return js;
}

void DbMessage::markReset(const char* toMark = NULL) {
    if (toMark == NULL) {
        toMark = _mark;
    }

    verify(toMark);
    _nextjsobj = toMark;
}

template <typename T>
void DbMessage::checkRead(const char* start, size_t count) const {
    if ((_theEnd - start) < static_cast<int>(sizeof(T) * count)) {
        uassert(18634, "Not enough data to read", false);
    }
}

template <typename T>
T DbMessage::read() const {
    checkRead<T>(_nextjsobj, 1);

    return ConstDataView(_nextjsobj).read<LittleEndian<T>>();
}

template <typename T>
T DbMessage::readAndAdvance() {
    T t = read<T>();
    _nextjsobj += sizeof(T);
    return t;
}

namespace {
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
}

Message makeInsertMessage(StringData ns, const BSONObj* objs, size_t count, int flags) {
    return makeMessage(dbInsert, [&](BufBuilder& b) {
        int reservedFlags = 0;
        if (flags & InsertOption_ContinueOnError)
            reservedFlags |= InsertOption_ContinueOnError;

        b.appendNum(reservedFlags);
        b.appendStr(ns);

        for (size_t i = 0; i < count; i++) {
            objs[i].appendSelfToBufBuilder(b);
        }
    });
}

Message makeUpdateMessage(StringData ns, BSONObj query, BSONObj update, int flags) {
    return makeMessage(dbUpdate, [&](BufBuilder& b) {
        const int reservedFlags = 0;
        b.appendNum(reservedFlags);
        b.appendStr(ns);
        b.appendNum(flags);

        query.appendSelfToBufBuilder(b);
        update.appendSelfToBufBuilder(b);
    });
}

Message makeRemoveMessage(StringData ns, BSONObj query, int flags) {
    return makeMessage(dbDelete, [&](BufBuilder& b) {
        const int reservedFlags = 0;
        b.appendNum(reservedFlags);
        b.appendStr(ns);
        b.appendNum(flags);

        query.appendSelfToBufBuilder(b);
    });
}

Message makeKillCursorsMessage(long long cursorId) {
    return makeMessage(dbKillCursors, [&](BufBuilder& b) {
        b.appendNum((int)0);  // reserved
        b.appendNum((int)1);  // number
        b.appendNum(cursorId);
    });
}

Message makeGetMoreMessage(StringData ns, long long cursorId, int nToReturn, int flags) {
    return makeMessage(dbGetMore, [&](BufBuilder& b) {
        b.appendNum(flags);
        b.appendStr(ns);
        b.appendNum(nToReturn);
        b.appendNum(cursorId);
    });
}

OpQueryReplyBuilder::OpQueryReplyBuilder() : _buffer(32768) {
    _buffer.skip(sizeof(QueryResult::Value));
}

Message OpQueryReplyBuilder::toQueryReply(int queryResultFlags,
                                          int nReturned,
                                          int startingFrom,
                                          long long cursorId) {
    QueryResult::View qr = _buffer.buf();
    qr.setResultFlags(queryResultFlags);
    qr.msgdata().setLen(_buffer.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(cursorId);
    qr.setStartingFrom(startingFrom);
    qr.setNReturned(nReturned);
    return Message(_buffer.release());
}

DbResponse replyToQuery(int queryResultFlags,
                        const void* data,
                        int size,
                        int nReturned,
                        int startingFrom,
                        long long cursorId) {
    OpQueryReplyBuilder reply;
    reply.bufBuilderForResults().appendBuf(data, size);
    return DbResponse{reply.toQueryReply(queryResultFlags, nReturned, startingFrom, cursorId)};
}
}
