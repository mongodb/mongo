/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/op_msg.h"

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/string_map.h"

namespace mongo {
namespace {
bool containsUnknownRequiredFlags(uint32_t flags) {
    const uint32_t kRequiredFlagMask = 0xffff;
    return uint32_t(flags & ~OpMsg::kAllKnownFlags & kRequiredFlagMask);
}

enum class Section : uint8_t {
    kBody = 0,
    kDocSequence = 1,
};
}  // namespace

OpMsg OpMsg::parse(const Message& message) try {
    // TODO some validation may make more sense in the IDL parser. I've tagged them with comments.
    OpMsg msg;
    // Use a separate BufReader for the flags since the flags can change how much room we have
    // for sections.
    BufReader(message.singleData().data(), message.dataSize()).read(msg.flags);
    uassert(40429,
            str::stream() << "Message contains illegal flags value: " << msg.flags,
            !containsUnknownRequiredFlags(msg.flags));

    invariant(!msg.isFlagSet(kChecksumPresent));  // TODO SERVER-28679 check checksum here.

    constexpr int kCrc32Size = 4;
    const int checksumSize = msg.isFlagSet(kChecksumPresent) ? kCrc32Size : 0;
    BufReader sectionsBuf(message.singleData().data() + sizeof(msg.flags),
                          message.dataSize() - sizeof(msg.flags) - checksumSize);
    bool haveBody = false;
    while (!sectionsBuf.atEof()) {
        const auto sectionKind = sectionsBuf.read<Section>();
        switch (sectionKind) {
            case Section::kBody: {
                uassert(40430, "Multiple body sections in message", !haveBody);
                haveBody = true;
                msg.body = sectionsBuf.read<Validated<BSONObj>>();
                break;
            }

            case Section::kDocSequence: {
                // The first 4 bytes are the total size, including themselves.
                const auto remainingSize = sectionsBuf.read<int32_t>() - sizeof(int32_t);
                BufReader seqBuf(sectionsBuf.skip(remainingSize), remainingSize);
                const auto name = seqBuf.readCStr();
                uassert(40431,
                        str::stream() << "Duplicate document sequence: " << name,
                        !msg.getSequence(name));  // TODO IDL

                msg.sequences.push_back({name.toString()});
                while (!seqBuf.atEof()) {
                    msg.sequences.back().objs.push_back(seqBuf.read<Validated<BSONObj>>());
                }
                break;
            }

            default:
                // Using uint32_t so we append as a decimal number rather than as a char.
                uasserted(40432, str::stream() << "Unknown section kind " << uint32_t(sectionKind));
        }
    }

    // Detect duplicates between doc sequences and body. TODO IDL
    // Technically this is O(N*M) but N is at most 2.
    for (const auto& docSeq : msg.sequences) {
        const char* name = docSeq.name.c_str();  // Pointer is redirected by next call.
        auto inBody =
            !dotted_path_support::extractElementAtPathOrArrayAlongPath(msg.body, name).eoo();
        uassert(40433,
                str::stream() << "Duplicate field between body and document sequence "
                              << docSeq.name,
                !inBody);
    }

    return msg;
} catch (const DBException& ex) {
    // TODO change to LOG(1).
    log() << "invalid message: " << redact(ex) << ' '
          << hexdump(message.singleData().view2ptr(), message.size());
    throw;
}

Message OpMsg::serialize() const {
    OpMsgBuilder builder;
    for (auto&& seq : sequences) {
        auto docSeq = builder.beginDocSequence(seq.name);
        for (auto&& obj : seq.objs) {
            docSeq.append(obj);
        }
    }
    builder.beginBody().appendElements(body);
    builder.flags() = flags;
    return builder.finish();
}

void OpMsg::shareOwnershipWith(const ConstSharedBuffer& buffer) {
    if (!body.isOwned()) {
        body.shareOwnershipWith(buffer);
    }
    for (auto&& seq : sequences) {
        for (auto&& obj : seq.objs) {
            if (!obj.isOwned()) {
                obj.shareOwnershipWith(buffer);
            }
        }
    }
}

auto OpMsgBuilder::beginDocSequence(StringData name) -> DocSequenceBuilder {
    invariant(_state == kEmpty || _state == kDocSequence);
    invariant(!_openBuilder);
    _state = kDocSequence;
    _buf.appendStruct(Section::kDocSequence);
    int sizeOffset = _buf.len();
    _buf.skip(sizeof(int32_t));  // section size.
    _buf.appendStr(name, true);
    return DocSequenceBuilder(this, &_buf, sizeOffset);
}

void OpMsgBuilder::finishDocumentStream(DocSequenceBuilder* docSequenceBuilder) {
    invariant(_state == kDocSequence);
    invariant(_openBuilder);
    _openBuilder = false;
    const int32_t size = _buf.len() - docSequenceBuilder->_sizeOffset;
    invariant(size > 0);
    DataView(_buf.buf()).write<LittleEndian<int32_t>>(size, docSequenceBuilder->_sizeOffset);
}

BSONObjBuilder OpMsgBuilder::beginBody() {
    invariant(_state == kEmpty || _state == kDocSequence);
    _state = kBody;
    _buf.appendStruct(Section::kBody);
    invariant(_bodyStart == 0);
    _bodyStart = _buf.len();  // Cannot be 0.
    return BSONObjBuilder(_buf);
}

BSONObjBuilder OpMsgBuilder::resumeBody() {
    invariant(_state == kBody);
    invariant(_bodyStart != 0);
    return BSONObjBuilder(BSONObjBuilder::ResumeBuildingTag(), _buf, _bodyStart);
}

Message OpMsgBuilder::finish() {
    invariant(_state == kBody);
    invariant(_bodyStart);
    invariant(!_openBuilder);
    _state = kDone;

    // TODO figure out where checksums should be calculated. It needs to be *after* we set the
    // messageID and replyTo fields in the header, which is currently done deep in the network
    // stack. That will either need to move closer to here, or the checksumming will need to be
    // done there.
    invariant(!(_flags & OpMsg::kChecksumPresent));  // TODO SERVER-28679 compute checksum.

    // If this fails, it means some internal user set an invalid flag.
    invariant(!containsUnknownRequiredFlags(_flags));

    DataView(_buf.buf())
        .write<LittleEndian<uint32_t>>(_flags, /*offset=*/sizeof(MSGHEADER::Layout));

    const auto size = _buf.len();
    MSGHEADER::View header(_buf.buf());
    header.setMessageLength(size);
    // header.setRequestMsgId(...); // These are currently filled in by the networking layer.
    // header.setResponseToMsgId(...);
    header.setOpCode(dbMsg);
    return Message(_buf.release());
}

}  // namespace mongo
