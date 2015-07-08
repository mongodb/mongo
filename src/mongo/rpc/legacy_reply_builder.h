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

#pragma once

#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/rpc/document_range.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_builder_interface.h"

namespace mongo {
namespace rpc {

class LegacyReplyBuilder : public ReplyBuilderInterface {
public:
    static const char kCursorTag[];
    static const char kFirstBatchTag[];

    LegacyReplyBuilder();
    LegacyReplyBuilder(std::unique_ptr<Message>);
    ~LegacyReplyBuilder() final;

    LegacyReplyBuilder& setMetadata(const BSONObj& metadata) final;
    LegacyReplyBuilder& setRawCommandReply(const BSONObj& commandReply) final;

    Status addOutputDocs(DocumentRange outputDocs) final;
    Status addOutputDoc(const BSONObj& outputDoc) final;

    State getState() const final;

    void reset() final;

    std::unique_ptr<Message> done() final;

    Protocol getProtocol() const final;

    std::size_t availableBytes() const final;

private:
    /**
     *  Checks if there is enough space in the buffer to store dataSize bytes
     *  and computes error message if not.
     */
    Status _hasSpaceFor(std::size_t dataSize) const;

    // If _allowAddingOutputDocs is false it enforces the "legacy" semantic
    // where command results are returned inside commandReply.
    // In this case calling addOutputDoc(s) will break the invariant.
    bool _allowAddingOutputDocs{true};
    BSONObj _commandReply{};
    std::size_t _currentLength;
    std::size_t _currentIndex = 0U;
    std::unique_ptr<Message> _message;
    BSONObj _metadata{};
    std::vector<BSONObj> _outputDocs{};
    State _state{State::kMetadata};
};

}  // namespace rpc
}  // namespace mongo
