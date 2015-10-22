/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/rpc/legacy_reply_builder.h"

#include <iterator>

#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/metadata.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace rpc {

namespace {
// Margin of error for availableBytes size estimate
std::size_t kReservedSize = 1024;

bool isEmptyCommandReply(const BSONObj& bson) {
    auto cursorElem = bson[LegacyReplyBuilder::kCursorTag];
    if (cursorElem.eoo())
        return true;

    BSONObj cursorObj = cursorElem.Obj();
    auto firstBatchElem = cursorObj[LegacyReplyBuilder::kFirstBatchTag];
    if (firstBatchElem.eoo())
        return true;

    BSONObjIterator it(firstBatchElem.Obj());

    return !it.more();
}
}  // namespace

const char LegacyReplyBuilder::kCursorTag[] = "cursor";
const char LegacyReplyBuilder::kFirstBatchTag[] = "firstBatch";

LegacyReplyBuilder::LegacyReplyBuilder() : LegacyReplyBuilder(Message()) {}

LegacyReplyBuilder::LegacyReplyBuilder(Message&& message)
    : _currentLength{kReservedSize}, _message{std::move(message)} {}

LegacyReplyBuilder::~LegacyReplyBuilder() {}

LegacyReplyBuilder& LegacyReplyBuilder::setMetadata(const BSONObj& metadata) {
    invariant(_state == State::kMetadata);

    auto dataSize = static_cast<std::size_t>(metadata.objsize());

    _currentLength += dataSize;
    _metadata = metadata.getOwned();
    _state = State::kCommandReply;
    return *this;
}

LegacyReplyBuilder& LegacyReplyBuilder::setRawCommandReply(const BSONObj& commandReply) {
    invariant(_state == State::kCommandReply);

    auto dataSize = static_cast<std::size_t>(commandReply.objsize());

    _currentLength += dataSize;
    _commandReply = commandReply.getOwned();
    _allowAddingOutputDocs = isEmptyCommandReply(_commandReply);

    _state = State::kOutputDocs;
    return *this;
}

Status LegacyReplyBuilder::addOutputDocs(DocumentRange docs) {
    invariant(_state == State::kOutputDocs);
    invariant(_allowAddingOutputDocs);

    auto dataSize = docs.data().length();

    auto hasSpace = _hasSpaceFor(dataSize);
    if (!hasSpace.isOK()) {
        return hasSpace;
    }

    // The temporary obj is used to address the case when where is not enough space.
    // BSONArray overhead can not be estimated upfront.
    std::vector<BSONObj> docsTmp{};
    std::size_t lenTmp = 0;
    std::size_t tmpIndex(_currentIndex);
    for (auto&& it : docs) {
        docsTmp.emplace_back(it.getOwned());
        lenTmp += BSONObjBuilder::numStr(++tmpIndex).length() + 2;  // space for storing array index
    }

    hasSpace = _hasSpaceFor(dataSize + lenTmp);
    if (!hasSpace.isOK()) {
        return hasSpace;
    }

    // vector::insert instead of swap allows to call addOutputDoc(s) multiple times
    _outputDocs.insert(_outputDocs.end(),
                       std::make_move_iterator(docsTmp.begin()),
                       std::make_move_iterator(docsTmp.end()));

    _currentIndex = tmpIndex;
    _currentLength += lenTmp;
    _currentLength += dataSize;
    return Status::OK();
}

Status LegacyReplyBuilder::addOutputDoc(const BSONObj& bson) {
    invariant(_state == State::kOutputDocs);
    invariant(_allowAddingOutputDocs);

    auto dataSize = static_cast<std::size_t>(bson.objsize());
    auto hasSpace = _hasSpaceFor(dataSize);
    if (!hasSpace.isOK()) {
        return hasSpace;
    }

    _outputDocs.emplace_back(bson.getOwned());
    _currentLength += dataSize;
    _currentLength += BSONObjBuilder::numStr(++_currentIndex).length() + 2;  // storing array index

    return Status::OK();
}

ReplyBuilderInterface::State LegacyReplyBuilder::getState() const {
    return _state;
}

Protocol LegacyReplyBuilder::getProtocol() const {
    return rpc::Protocol::kOpQuery;
}

void LegacyReplyBuilder::reset() {
    // If we are in State::kMetadata, we are already in the 'start' state, so by
    // immediately returning, we save a heap allocation.
    if (_state == State::kMetadata) {
        return;
    }
    _message.reset();
    _currentLength = kReservedSize;
    _currentIndex = 0U;
    _state = State::kMetadata;
}


Message LegacyReplyBuilder::done() {
    invariant(_state == State::kOutputDocs);

    BSONObj reply = uassertStatusOK(rpc::downconvertReplyMetadata(_commandReply, _metadata));

    BufBuilder bufBuilder;
    bufBuilder.skip(sizeof(QueryResult::Value));

    if (_allowAddingOutputDocs) {
        BSONObjBuilder topBuilder(bufBuilder);
        for (const auto& el : reply) {
            if (kCursorTag != el.fieldNameStringData()) {
                topBuilder.append(el);
                continue;
            }
            invariant(el.isABSONObj());
            BSONObjBuilder curBuilder(topBuilder.subobjStart(kCursorTag));
            for (const auto& insideEl : el.Obj()) {
                if (kFirstBatchTag != insideEl.fieldNameStringData()) {
                    curBuilder.append(insideEl);
                    continue;
                }
                invariant(insideEl.isABSONObj());
                BSONArrayBuilder arrBuilder(curBuilder.subarrayStart(kFirstBatchTag));
                for (const auto& doc : _outputDocs) {
                    arrBuilder.append(doc);
                }
                arrBuilder.doneFast();
            }
            curBuilder.doneFast();
        }
        topBuilder.doneFast();
    } else {
        reply.appendSelfToBufBuilder(bufBuilder);
    }

    auto msgHeaderSz = static_cast<std::size_t>(MsgData::MsgDataHeaderSize);

    invariant(static_cast<std::size_t>(bufBuilder.len()) + msgHeaderSz <=
              mongo::MaxMessageSizeBytes);

    QueryResult::View qr = bufBuilder.buf();

    qr.setResultFlagsToOk();
    qr.msgdata().setLen(bufBuilder.len());
    qr.msgdata().setOperation(opReply);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(1);

    _message.setData(qr.view2ptr(), true);
    bufBuilder.decouple();

    _state = State::kDone;
    return std::move(_message);
}

std::size_t LegacyReplyBuilder::availableBytes() const {
    std::size_t msgHeaderSz = static_cast<std::size_t>(MsgData::MsgDataHeaderSize);
    return mongo::MaxMessageSizeBytes - _currentLength - msgHeaderSz;
}

Status LegacyReplyBuilder::_hasSpaceFor(std::size_t dataSize) const {
    size_t availBytes = availableBytes();
    if (availBytes < dataSize) {
        return Status(ErrorCodes::Overflow,
                      str::stream() << "Not enough space to store " << dataSize << " bytes. Only "
                                    << availBytes << " bytes are available.");
    }
    return Status::OK();
}

}  // namespace rpc
}  // namespace mongo
