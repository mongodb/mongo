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

#pragma once

#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/protocol.h"

namespace mongo {
class BSONObj;
class BSONObjBuilder;
class Message;

namespace rpc {

/**
 * Constructs an RPC Reply.
 */
class ReplyBuilderInterface {
    ReplyBuilderInterface(const ReplyBuilderInterface&) = delete;
    ReplyBuilderInterface& operator=(const ReplyBuilderInterface&) = delete;

public:
    virtual ~ReplyBuilderInterface() = default;

    /**
     * Sets the raw command reply. This should probably not be used in favor of the
     * variants that accept a Status or StatusWith.
     */
    virtual ReplyBuilderInterface& setRawCommandReply(const BSONObj& reply) = 0;

    /**
     * Returns a BSONObjBuilder that can be used to build the reply in-place. The returned
     * builder (or an object into which it has been moved) must be completed before calling
     * any more methods on this object. A builder is completed by a call to `done()` or by
     * its destructor. Can be called repeatedly to append multiple things to the reply, as
     * long as each returned builder is completed between calls.
     */
    virtual BSONObjBuilder getBodyBuilder() = 0;

    /**
     * Returns a DocSeqBuilder for building a command reply in place. This should only be called
     * before the body as the body will have status types appended at the end.
     */
    virtual OpMsgBuilder::DocSequenceBuilder getDocSequenceBuilder(StringData name) {
        uasserted(50875, "Only OpMsg may use document sequences");
    }

    /**
     * Sets the reply for this command. If an engaged StatusWith<BSONObj> is passed, the command
     * reply will be set to the contained BSONObj, augmented with the element {ok, 1.0} if it
     * does not already have an "ok" field. If a disengaged StatusWith<BSONObj> is passed, the
     * command reply will be set to {ok: 0.0, code: <code of status>,
     *                               codeName: <name of status code>,
     *                               errmsg: <reason of status>}
     */
    ReplyBuilderInterface& setCommandReply(StatusWith<BSONObj> commandReply);

    /**
     * Sets the reply for this command. The status parameter must be non-OK. The reply for
     * this command will be set to an object containing all the fields in extraErrorInfo,
     * augmented with {ok: 0.0} , {code: <code of status>}, {codeName: <name of status code>},
     * and {errmsg: <reason of status>}.
     * If any of the fields "ok", "code", or "errmsg" already exist in extraErrorInfo, they
     * will be left as-is in the command reply. This use of this form is intended for
     * interfacing with legacy code that adds additional data to a failed command reply and
     * its use is discouraged in new code.
     */
    virtual ReplyBuilderInterface& setCommandReply(Status nonOKStatus, BSONObj extraErrorInfo);

    /**
     * Gets the protocol used to serialize this reply. This should be used for validity checks
     * only - runtime behavior changes should be implemented with polymorphism.
     */
    virtual Protocol getProtocol() const = 0;

    /**
     * Resets the state of the builder to kMetadata and clears any data that was previously
     * written.
     */
    virtual void reset() = 0;

    /**
     * Writes data then transfers ownership of the message to the caller. The behavior of
     * calling any methods on the builder is subsequently undefined.
     */
    virtual Message done() = 0;

    /**
     * The specified 'object' must be BSON-serializable.
     *
     * BSONSerializable 'x' means 'x.serialize(bob)' appends a representation of 'x'
     * into 'BSONObjBuilder* bob'.
     */
    template <typename T>
    void fillFrom(const T& object) {
        static_assert(!isStatusOrStatusWith<std::decay_t<T>>,
                      "Status and StatusWith<T> aren't supported by TypedCommand and fillFrom(). "
                      "Use uassertStatusOK() instead.");
        auto bob = getBodyBuilder();
        object.serialize(&bob);
    }

    /**
     * Reserves and claims bytes for the Message generated by this interface.
     */
    virtual void reserveBytes(const std::size_t bytes) = 0;

protected:
    ReplyBuilderInterface() = default;
};

}  // namespace rpc
}  // namespace mongo
