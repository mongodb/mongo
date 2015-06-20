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
#include <utility>

#include "mongo/base/disallow_copying.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/request_interface.h"

namespace mongo {
class Message;
namespace rpc {

/**
 * A wrapper around an owned message that includes access to an associated
 * ReplyInterface or RequestInterface.
 */
template <typename MessageViewType>
class UniqueMessage {
    MONGO_DISALLOW_COPYING(UniqueMessage);

public:
    UniqueMessage(std::unique_ptr<Message> message, std::unique_ptr<MessageViewType> view)
        : _message{std::move(message)}, _view{std::move(view)} {}

#if defined(_MSC_VER) && _MSC_VER < 1900
    UniqueMessage(UniqueMessage&& other)
        : _message{std::move(other._message)}, _view{std::move(other._view)} {}

    UniqueMessage& operator=(UniqueMessage&& other) {
        _message = std::move(other._message);
        _view = std::move(other._view);
        return *this;
    }
#else
    UniqueMessage(UniqueMessage&&) = default;
    UniqueMessage& operator=(UniqueMessage&&) = default;
#endif

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
    std::unique_ptr<Message> releaseMessage() {
        _view.reset();
        return std::move(_message);
    }

private:
    std::unique_ptr<Message> _message;
    std::unique_ptr<MessageViewType> _view;
};

using UniqueReply = UniqueMessage<ReplyInterface>;
using UniqueRequest = UniqueMessage<RequestInterface>;

}  // namespace rpc
}  // namespace mongo
