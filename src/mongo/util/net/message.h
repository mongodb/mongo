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
#include <vector>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/base/encoded_value_storage.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/allocator.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/print.h"

namespace mongo {

/**
 * Maximum accepted message size on the wire protocol.
 */
const size_t MaxMessageSizeBytes = 48 * 1000 * 1000;

class Message;
class MessagingPort;

typedef uint32_t MSGID;

enum NetworkOp {
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

    int32_t getRequestID() const {
        return data().read<LittleEndian<int32_t>>(offsetof(Layout, requestID));
    }

    int32_t getResponseTo() const {
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

    void setRequestID(int32_t value) {
        data().write(tagLittleEndian(value), offsetof(Layout, requestID));
    }

    void setResponseTo(int32_t value) {
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

    MSGID getId() const {
        return header().getRequestID();
    }

    MSGID getResponseTo() const {
        return header().getResponseTo();
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
        verify(getResponseTo() > 0);
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

    void setId(MSGID value) {
        return header().setRequestID(value);
    }

    void setResponseTo(MSGID value) {
        return header().setResponseTo(value);
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
    MONGO_DISALLOW_COPYING(Message);

public:
    // we assume here that a vector with initial size 0 does no allocation (0 is the default, but
    // wanted to make it explicit).
    Message() = default;

    Message(void* data, bool freeIt) : _buf(reinterpret_cast<char*>(data)), _freeIt(freeIt) {}

    Message(Message&& r) : _buf(r._buf), _data(std::move(r._data)), _freeIt(r._freeIt) {
        r._buf = nullptr;
        r._freeIt = false;
    }

    ~Message() {
        reset();
    }

    SockAddr _from;

    MsgData::View header() const {
        verify(!empty());
        return _buf ? _buf : _data[0].first;
    }

    NetworkOp operation() const {
        return header().getNetworkOp();
    }

    MsgData::View singleData() const {
        massert(13273, "single data buffer expected", _buf);
        return header();
    }

    bool empty() const {
        return !_buf && _data.empty();
    }

    int size() const {
        int res = 0;
        if (_buf) {
            res = MsgData::ConstView(_buf).getLen();
        } else {
            for (MsgVec::const_iterator it = _data.begin(); it != _data.end(); ++it) {
                res += it->second;
            }
        }
        return res;
    }

    int dataSize() const {
        return size() - sizeof(MSGHEADER::Value);
    }

    // concat multiple buffers - noop if <2 buffers already, otherwise can be expensive copy
    // can get rid of this if we make response handling smarter
    void concat() {
        if (_buf || empty()) {
            return;
        }

        verify(_freeIt);
        int totalSize = 0;
        for (std::vector<std::pair<char*, int>>::const_iterator i = _data.begin(); i != _data.end();
             ++i) {
            totalSize += i->second;
        }
        char* buf = (char*)mongoMalloc(totalSize);
        char* p = buf;
        for (std::vector<std::pair<char*, int>>::const_iterator i = _data.begin(); i != _data.end();
             ++i) {
            memcpy(p, i->first, i->second);
            p += i->second;
        }
        reset();
        _setData(buf, true);
    }

    Message& operator=(Message&& r) {
        // This implementation was originally written using an auto_ptr-style fake move. When
        // converting to a real move assignment, checking for self-assignment was the simplest way
        // to ensure correctness.
        if (&r == this)
            return *this;

        if (!empty()) {
            reset();
        }

        _buf = r._buf;
        _data = std::move(r._data);
        _freeIt = r._freeIt;

        r._buf = nullptr;
        r._freeIt = false;
        return *this;
    }

    void reset() {
        if (_freeIt) {
            if (_buf) {
                std::free(_buf);
            }
            for (std::vector<std::pair<char*, int>>::const_iterator i = _data.begin();
                 i != _data.end();
                 ++i) {
                std::free(i->first);
            }
        }
        _buf = nullptr;
        _data.clear();
        _freeIt = false;
    }

    // use to add a buffer
    // assumes message will free everything
    void appendData(char* d, int size) {
        if (size <= 0) {
            return;
        }
        if (empty()) {
            MsgData::View md = d;
            md.setLen(size);  // can be updated later if more buffers added
            _setData(md.view2ptr(), true);
            return;
        }
        verify(_freeIt);
        if (_buf) {
            _data.push_back(std::make_pair(_buf, MsgData::ConstView(_buf).getLen()));
            _buf = 0;
        }
        _data.push_back(std::make_pair(d, size));
        header().setLen(header().getLen() + size);
    }

    // use to set first buffer if empty
    void setData(char* d, bool freeIt) {
        verify(empty());
        _setData(d, freeIt);
    }
    void setData(int operation, const char* msgtxt) {
        setData(operation, msgtxt, strlen(msgtxt) + 1);
    }
    void setData(int operation, const char* msgdata, size_t len) {
        verify(empty());
        size_t dataLen = len + sizeof(MsgData::Value) - 4;
        MsgData::View d = reinterpret_cast<char*>(mongoMalloc(dataLen));
        memcpy(d.data(), msgdata, len);
        d.setLen(dataLen);
        d.setOperation(operation);
        _setData(d.view2ptr(), true);
    }

    bool doIFreeIt() {
        return _freeIt;
    }

    char* buf() {
        return _buf;
    }

    void send(MessagingPort& p, const char* context);

    std::string toString() const;

private:
    void _setData(char* d, bool freeIt) {
        _freeIt = freeIt;
        _buf = d;
    }
    // if just one buffer, keep it in _buf, otherwise keep a sequence of buffers in _data
    char* _buf{nullptr};
    // byte buffer(s) - the first must contain at least a full MsgData unless using _buf for storage
    // instead
    typedef std::vector<std::pair<char*, int>> MsgVec;
    MsgVec _data{};
    bool _freeIt{false};
};


MSGID nextMessageId();


}  // namespace mongo
