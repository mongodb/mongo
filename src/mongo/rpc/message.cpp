// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/message.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/platform/atomic.h"
#include "mongo/rpc/op_msg.h"

#include <ostream>
#include <vector>

#include <fmt/format.h>

namespace mongo {

namespace {
Atomic<int32_t> NextMsgId;
}  // namespace

int32_t nextMessageId() {
    return NextMsgId.fetchAndAdd(1);
}

void Message::setData(int operation, const char* msgdata, size_t len) {
    const size_t dataLen = sizeof(MsgData::Value) + len;
    auto buf = SharedBuffer::allocate(dataLen);
    MsgData::View d = buf.get();
    d.setLen(dataLen);
    d.setOperation(operation);
    if (len)
        memcpy(d.data(), msgdata, len);
    setData(std::move(buf));
}

std::string Message::opMsgDebugString() const {
    MsgData::ConstView headerView = header();
    auto opMsgRequest = OpMsgRequest::parse(*this);
    std::stringstream docSequences;
    int idx = 0;
    for (const auto& seq : opMsgRequest.sequences) {
        docSequences << fmt::format("Sequence Idx: {} Sequence Name: {}", idx++, seq.name)
                     << std::endl;
        for (const auto& obj : seq.objs) {
            docSequences << fmt::format("\t{}", obj.toString()) << std::endl;
        }
    }

    return fmt::format(
        "Length: {} RequestId: {} ResponseTo: {} OpCode: {} Flags: {} Body: {}\n"
        "Sections: {}",
        headerView.getLen(),
        headerView.getId(),
        headerView.getResponseToMsgId(),
        fmt::underlying(headerView.getNetworkOp()),
        OpMsg::flags(*this),
        opMsgRequest.body.toString(),
        docSequences.str());
}

}  // namespace mongo
