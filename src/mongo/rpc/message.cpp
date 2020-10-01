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

#include "mongo/platform/basic.h"

#include "mongo/rpc/message.h"

#include <fmt/format.h>

#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_msg.h"

namespace mongo {

namespace {
AtomicWord<int32_t> NextMsgId;
}  // namespace

int32_t nextMessageId() {
    return NextMsgId.fetchAndAdd(1);
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
        headerView.getNetworkOp(),
        OpMsg::flags(*this),
        opMsgRequest.body.toString(),
        docSequences.str());
}

}  // namespace mongo
