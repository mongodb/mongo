// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/rpc/message.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] rpc {

/**
 * A wrapper around an owned message that includes access to an associated ReplyInterface.
 */
template <typename MessageViewType>
class UniqueMessage {
    UniqueMessage(const UniqueMessage&) = delete;
    UniqueMessage& operator=(const UniqueMessage&) = delete;

public:
    UniqueMessage(Message message, std::unique_ptr<MessageViewType> view)
        : _message{std::move(message)}, _view{std::move(view)} {}

    UniqueMessage(UniqueMessage&&) = default;
    UniqueMessage& operator=(UniqueMessage&&) = default;

    const MessageViewType* operator->() const {
        return _view.get();
    }

    const MessageViewType& operator*() const {
        return *_view;
    }

    /**
     * Releases ownership of the underlying message. The result of calling any other methods
     * on the object afterward is undefined.
     */
    Message releaseMessage() {
        _view.reset();
        return std::move(_message);
    }

private:
    Message _message;
    std::unique_ptr<MessageViewType> _view;
};

using UniqueReply = UniqueMessage<ReplyInterface>;

}  // namespace rpc
}  // namespace mongo
