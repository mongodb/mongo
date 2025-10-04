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

#include "mongo/db/dbmessage.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/strnlen.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstring>

namespace mongo {

DbMessage::DbMessage(const Message& msg) : _msg(msg), _nsStart(nullptr), _mark(nullptr), _nsLen(0) {
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
    MONGO_verify(messageShouldHaveNs());
    return _nsStart;
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
            _nextjsobj != nullptr && _theEnd - _nextjsobj >= 5);

    if (serverGlobalParams.objcheck) {
        Status status = validateBSON(_nextjsobj, _theEnd - _nextjsobj);
        uassert(ErrorCodes::InvalidBSON,
                str::stream() << "Client Error: bad object in message: " << status.reason(),
                status.isOK());
    }

    BSONObj js(_nextjsobj);
    MONGO_verify(js.objsize() >= 5);
    MONGO_verify(js.objsize() <= (_theEnd - _nextjsobj));

    _nextjsobj += js.objsize();
    if (_nextjsobj >= _theEnd)
        _nextjsobj = nullptr;
    return js;
}

void DbMessage::markReset(const char* toMark = nullptr) {
    if (toMark == nullptr) {
        toMark = _mark;
    }

    MONGO_verify(toMark);
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

Message makeUnsupportedOpInsertMessage(StringData ns,
                                       const BSONObj* objs,
                                       size_t count,
                                       int flags) {
    return makeMessage(dbInsert, [&](BufBuilder& b) {
        int reservedFlags = 0;
        if (flags & InsertOption_ContinueOnError)
            reservedFlags |= InsertOption_ContinueOnError;

        b.appendNum(reservedFlags);
        b.appendCStr(ns);

        for (size_t i = 0; i < count; i++) {
            objs[i].appendSelfToBufBuilder(b);
        }
    });
}

DbResponse makeErrorResponseToUnsupportedOpQuery(StringData errorMsg) {
    BSONObjBuilder err;
    err.append("$err",
               str::stream() << errorMsg
                             << ". The client driver may require an upgrade. For more details see "
                                "https://dochub.mongodb.org/core/legacy-opcode-removal");
    err.append("code", 5739101);
    err.append("ok", 0.0);
    BSONObj errObj = err.done();

    BufBuilder buffer(sizeof(QueryResult::Value) + errObj.objsize());
    buffer.skip(sizeof(QueryResult::Value));
    buffer.appendBuf(errObj.objdata(), errObj.objsize());

    QueryResult::View qr = buffer.buf();
    qr.msgdata().setLen(buffer.len());
    qr.msgdata().setOperation(opReply);
    qr.setResultFlags(ResultFlag_ErrSet);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);

    return DbResponse{Message(buffer.release())};
}
}  // namespace mongo
