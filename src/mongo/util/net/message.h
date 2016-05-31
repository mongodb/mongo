// message.h

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

#include <cstdint>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/encoded_value_storage.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/allocator.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/print.h"

namespace mongo {

/**
 * Maximum accepted message size on the wire protocol.
 */
const size_t MaxMessageSizeBytes = 48 * 1000 * 1000;

class Message;

enum NetworkOp : int32_t {
    opInvalid = 0,
    opReply = 1,     /* reply. responseTo is set. */
    dbMsg = 1000,    /* generic msg command followed by a std::string */
    dbUpdate = 2001, /* update object */
    dbInsert = 2002,
    // dbGetByOID = 2003,
    dbQuery = 2004,
    dbGetMore = 2005,
    dbDelete = 2006,
    dbKillCursors = 2007,
    // dbCommand_DEPRECATED = 2008, //
    // dbCommandReply_DEPRECATED = 2009, //
    dbCommand = 2010,
    dbCommandReply = 2011,
};

enum class LogicalOp {
    opInvalid,
    opMsg,
    opUpdate,
    opInsert,
    opQuery,
    opGetMore,
    opDelete,
    opKillCursors,
    opCommand,
};

static inline LogicalOp networkOpToLogicalOp(NetworkOp networkOp) {
    switch (networkOp) {
        case dbMsg:
            return LogicalOp::opMsg;
        case dbUpdate:
            return LogicalOp::opUpdate;
        case dbInsert:
            return LogicalOp::opInsert;
        case dbQuery:
            return LogicalOp::opQuery;
        case dbGetMore:
            return LogicalOp::opGetMore;
        case dbDelete:
            return LogicalOp::opDelete;
        case dbKillCursors:
            return LogicalOp::opKillCursors;
        case dbCommand:
            return LogicalOp::opCommand;
        default:
            int op = int(networkOp);
            massert(34348, str::stream() << "cannot translate opcode " << op, !op);
            return LogicalOp::opInvalid;
    }
}

bool doesOpGetAResponse(int op);

inline const char* networkOpToString(NetworkOp networkOp) {
    switch (networkOp) {
        case opInvalid:
            return "none";
        case opReply:
            return "reply";
        case dbMsg:
            return "msg";
        case dbUpdate:
            return "update";
        case dbInsert:
            return "insert";
        case dbQuery:
            return "query";
        case dbGetMore:
            return "getmore";
        case dbDelete:
            return "remove";
        case dbKillCursors:
            return "killcursors";
        case dbCommand:
            return "command";
        case dbCommandReply:
            return "commandReply";
        default:
            int op = static_cast<int>(networkOp);
            massert(16141, str::stream() << "cannot translate opcode " << op, !op);
            return "";
    }
}

inline const char* logicalOpToString(LogicalOp logicalOp) {
    switch (logicalOp) {
        case LogicalOp::opInvalid:
            return "none";
        case LogicalOp::opMsg:
            return "msg";
        case LogicalOp::opUpdate:
            return "update";
        case LogicalOp::opInsert:
            return "insert";
        case LogicalOp::opQuery:
            return "query";
        case LogicalOp::opGetMore:
            return "getmore";
        case LogicalOp::opDelete:
            return "remove";
        case LogicalOp::opKillCursors:
            return "killcursors";
        case LogicalOp::opCommand:
            return "command";
        default:
            MONGO_UNREACHABLE;
    }
}

inline bool opIsWrite(int op) {
    switch (op) {
        case 0:
        case opReply:
        case dbMsg:
        case dbQuery:
        case dbGetMore:
        case dbKillCursors:
            return false;

        case dbUpdate:
        case dbInsert:
        case dbDelete:
            return true;

        default:
            PRINT(op);
            verify(0);
            return "";
    }
}

namespace MSGHEADER {
#pragma pack(1)
/* see http://dochub.mongodb.org/core/mongowireprotocol
*/
struct Layout {
    int32_t messageLength;  // total message size, including this
    int32_t requestID;      // identifier for this message
    int32_t responseTo;     // requestID from the original request
    //   (used in responses from db)
    int32_t opCode;
};
#pragma pack()

class ConstView {
public:
    typedef ConstDataView view_type;

    ConstView(const char* data) : _data(data) {}

    const char* view2ptr() const {
        return data().view();
    }

    int32_t getMessageLength() const {
        return data().read<LittleEndian<int32_t>>(offsetof(Layout, messageLength));
    }

    int32_t getRequestMsgId() const {
        return data().read<LittleEndian<int32_t>>(offsetof(Layout, requestID));
    }

    int32_t getResponseToMsgId() const {
        return data().read<LittleEndian<int32_t>>(offsetof(Layout, responseTo));
    }

    int32_t getOpCode() const {
        return data().read<LittleEndian<int32_t>>(offsetof(Layout, opCode));
    }

protected:
    const view_type& data() const {
        return _data;
    }

private:
    view_type _data;
};

class View : public ConstView {
public:
    typedef DataView view_type;

    View(char* data) : ConstView(data) {}

    using ConstView::view2ptr;
    char* view2ptr() {
        return data().view();
    }

    void setMessageLength(int32_t value) {
        data().write(tagLittleEndian(value), offsetof(Layout, messageLength));
    }

    void setRequestMsgId(int32_t value) {
        data().write(tagLittleEndian(value), offsetof(Layout, requestID));
    }

    void setResponseToMsgId(int32_t value) {
        data().write(tagLittleEndian(value), offsetof(Layout, responseTo));
    }

    void setOpCode(int32_t value) {
        data().write(tagLittleEndian(value), offsetof(Layout, opCode));
    }

private:
    view_type data() const {
        return const_cast<char*>(ConstView::view2ptr());
    }
};

class Value : public EncodedValueStorage<Layout, ConstView, View> {
public:
    Value() {
        static_assert(sizeof(Value) == sizeof(Layout), "sizeof(Value) == sizeof(Layout)");
    }

    Value(ZeroInitTag_t zit) : EncodedValueStorage<Layout, ConstView, View>(zit) {}
};

}  // namespace MSGHEADER

namespace MsgData {
#pragma pack(1)
struct Layout {
    MSGHEADER::Layout header;
    char data[4];
};
#pragma pack()

class ConstView {
public:
    ConstView(const char* storage) : _storage(storage) {}

    const char* view2ptr() const {
        return storage().view();
    }

    int32_t getLen() const {
        return header().getMessageLength();
    }

    int32_t getId() const {
        return header().getRequestMsgId();
    }

    int32_t getResponseToMsgId() const {
        return header().getResponseToMsgId();
    }

    NetworkOp getNetworkOp() const {
        return NetworkOp(header().getOpCode());
    }

    const char* data() const {
        return storage().view(offsetof(Layout, data));
    }

    bool valid() const {
        if (getLen() <= 0 || getLen() > (4 * BSONObjMaxInternalSize))
            return false;
        if (getNetworkOp() < 0 || getNetworkOp() > 30000)
            return false;
        return true;
    }

    int64_t getCursor() const {
        verify(getResponseToMsgId() > 0);
        verify(getNetworkOp() == opReply);
        return ConstDataView(data() + sizeof(int32_t)).read<LittleEndian<int64_t>>();
    }

    int dataLen() const;  // len without header

protected:
    const ConstDataView& storage() const {
        return _storage;
    }

    MSGHEADER::ConstView header() const {
        return storage().view(offsetof(Layout, header));
    }

private:
    ConstDataView _storage;
};

class View : public ConstView {
public:
    View(char* storage) : ConstView(storage) {}

    using ConstView::view2ptr;
    char* view2ptr() {
        return storage().view();
    }

    void setLen(int value) {
        return header().setMessageLength(value);
    }

    void setId(int32_t value) {
        return header().setRequestMsgId(value);
    }

    void setResponseToMsgId(int32_t value) {
        return header().setResponseToMsgId(value);
    }

    void setOperation(int value) {
        return header().setOpCode(value);
    }

    using ConstView::data;
    char* data() {
        return storage().view(offsetof(Layout, data));
    }

private:
    DataView storage() const {
        return const_cast<char*>(ConstView::view2ptr());
    }

    MSGHEADER::View header() const {
        return storage().view(offsetof(Layout, header));
    }
};

class Value : public EncodedValueStorage<Layout, ConstView, View> {
public:
    Value() {
        static_assert(sizeof(Value) == sizeof(Layout), "sizeof(Value) == sizeof(Layout)");
    }

    Value(ZeroInitTag_t zit) : EncodedValueStorage<Layout, ConstView, View>(zit) {}
};

const int MsgDataHeaderSize = sizeof(Value) - 4;
inline int ConstView::dataLen() const {
    return getLen() - MsgDataHeaderSize;
}
}  // namespace MsgData

class Message {
public:
    Message() = default;
    explicit Message(SharedBuffer data) : _buf(std::move(data)) {}

    MsgData::View header() const {
        verify(!empty());
        return _buf.get();
    }

    NetworkOp operation() const {
        return header().getNetworkOp();
    }

    MsgData::View singleData() const {
        massert(13273, "single data buffer expected", _buf);
        return header();
    }

    bool empty() const {
        return !_buf;
    }

    int size() const {
        if (_buf) {
            return MsgData::ConstView(_buf.get()).getLen();
        }
        return 0;
    }

    int dataSize() const {
        return size() - sizeof(MSGHEADER::Value);
    }

    void reset() {
        _buf = {};
    }

    // use to set first buffer if empty
    void setData(SharedBuffer buf) {
        verify(empty());
        _buf = std::move(buf);
    }
    void setData(int operation, const char* msgtxt) {
        setData(operation, msgtxt, strlen(msgtxt) + 1);
    }
    void setData(int operation, const char* msgdata, size_t len) {
        verify(empty());
        size_t dataLen = len + sizeof(MsgData::Value) - 4;
        _buf = SharedBuffer::allocate(dataLen);
        MsgData::View d = _buf.get();
        if (len)
            memcpy(d.data(), msgdata, len);
        d.setLen(dataLen);
        d.setOperation(operation);
    }

    char* buf() {
        return _buf.get();
    }

    const char* buf() const {
        return _buf.get();
    }

    std::string toString() const;

    SharedBuffer sharedBuffer() {
        return _buf;
    }

    ConstSharedBuffer sharedBuffer() const {
        return _buf;
    }

private:
    SharedBuffer _buf;
};


int32_t nextMessageId();


}  // namespace mongo
