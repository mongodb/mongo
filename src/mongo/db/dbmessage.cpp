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
#include "mongo/db/operation_context.h"
#include "mongo/platform/strnlen.h"
#include "mongo/transport/session.h"

namespace mongo {

using std::string;
using std::stringstream;

string Message::toString() const {
    stringstream ss;
    ss << "op: " << networkOpToString(operation()) << " len: " << size();
    if (operation() >= 2000 && operation() < 2100) {
        DbMessage d(*this);
        ss << " ns: " << d.getns();
        switch (operation()) {
            case dbUpdate: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                BSONObj o = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q << " update: " << o;
                break;
            }
            case dbInsert:
                ss << d.nextJsObj();
                break;
            case dbDelete: {
                int flags = d.pullInt();
                BSONObj q = d.nextJsObj();
                ss << " flags: " << flags << " query: " << q;
                break;
            }
            default:
                ss << " CANNOT HANDLE YET";
        }
    }
    return ss.str();
}

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
    massert(10304,
            "Client Error: Remaining data too small for BSON object",
            _nextjsobj != NULL && _theEnd - _nextjsobj >= 5);

    if (serverGlobalParams.objcheck) {
        Status status = validateBSON(_nextjsobj, _theEnd - _nextjsobj);
        massert(10307,
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

OpQueryReplyBuilder::OpQueryReplyBuilder() : _buffer(32768) {
    _buffer.skip(sizeof(QueryResult::Value));
}

void OpQueryReplyBuilder::send(transport::Session* session,
                               int queryResultFlags,
                               const Message& requestMsg,
                               int nReturned,
                               int startingFrom,
                               long long cursorId) {
    Message response;
    putInMessage(&response, queryResultFlags, nReturned, startingFrom, cursorId);

    response.header().setId(nextMessageId());
    response.header().setResponseToMsgId(requestMsg.header().getId());

    uassertStatusOK(session->sinkMessage(response).wait());
}

void OpQueryReplyBuilder::sendCommandReply(transport::Session* session, const Message& requestMsg) {
    send(session, /*queryFlags*/ 0, requestMsg, /*nReturned*/ 1);
}

void OpQueryReplyBuilder::putInMessage(
    Message* out, int queryResultFlags, int nReturned, int startingFrom, long long cursorId) {
    QueryResult::View qr = _buffer.buf();
    qr.setResultFlags(queryResultFlags);
    qr.msgdata().setLen(_buffer.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(cursorId);
    qr.setStartingFrom(startingFrom);
    qr.setNReturned(nReturned);
    out->setData(_buffer.release());  // transport will free
}

void replyToQuery(int queryResultFlags,
                  transport::Session* session,
                  Message& requestMsg,
                  const void* data,
                  int size,
                  int nReturned,
                  int startingFrom,
                  long long cursorId) {
    OpQueryReplyBuilder reply;
    reply.bufBuilderForResults().appendBuf(data, size);
    reply.send(session, queryResultFlags, requestMsg, nReturned, startingFrom, cursorId);
}

void replyToQuery(int queryResultFlags,
                  transport::Session* session,
                  Message& requestMsg,
                  const BSONObj& responseObj) {
    replyToQuery(queryResultFlags,
                 session,
                 requestMsg,
                 (void*)responseObj.objdata(),
                 responseObj.objsize(),
                 1);
}

void replyToQuery(int queryResultFlags, Message& m, DbResponse& dbresponse, BSONObj obj) {
    Message resp;
    replyToQuery(queryResultFlags, resp, obj);
    dbresponse.response = std::move(resp);
    dbresponse.responseToMsgId = m.header().getId();
}

void replyToQuery(int queryResultFlags, Message& response, const BSONObj& resultObj) {
    OpQueryReplyBuilder reply;
    resultObj.appendSelfToBufBuilder(reply.bufBuilderForResults());
    reply.putInMessage(&response, queryResultFlags, /*nReturned*/ 1);
}
}
