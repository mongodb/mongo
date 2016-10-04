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

#include "mongo/rpc/protocol.h"

#include <memory>

/**
 * Utilities to construct the correct concrete rpc class based on what the remote server
 * supports, and what the client has been configured to do.
 */

namespace mongo {
class Message;

namespace rpc {
class ReplyBuilderInterface;
class ReplyInterface;
class RequestBuilderInterface;
class RequestInterface;

/**
 * Returns the appropriate concrete RequestBuilder. Throws if one cannot be chosen.
 */
std::unique_ptr<RequestBuilderInterface> makeRequestBuilder(ProtocolSet clientProtos,
                                                            ProtocolSet serverProtos);

std::unique_ptr<RequestBuilderInterface> makeRequestBuilder(Protocol proto);

/**
 * Returns the appropriate concrete Reply according to the contents of the message.
 * Throws if one cannot be chosen.
 */
std::unique_ptr<ReplyInterface> makeReply(const Message* unownedMessage);

/**
 * Returns the appropriate concrete Request according to the contents of the message.
 * Throws if one cannot be chosen.
 */
std::unique_ptr<RequestInterface> makeRequest(const Message* unownedMessage);

/**
 * Returns the appropriate concrete ReplyBuilder.
 */
std::unique_ptr<ReplyBuilderInterface> makeReplyBuilder(Protocol protocol);

}  // namespace rpc
}  // namespace mongo
