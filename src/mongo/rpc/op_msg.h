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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/tenant_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/message.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {

/**
 * OpMsg packets are made up of the following sequence of possible fields.
 *
 * ----------------------------
 * uint32_t           flags;     // One or more of the flags fields defined below
 *                               // Bits 0-15 MUST be supported by the receiving peer
 *                               // Bits 16-31 are OPTIONAL and may be ignored
 * DocumentSequence[] docs;      // Zero or more name/BSON pairs describing the message
 * optional<uint32_t> checksum;  // CRC-32C checksum for the preceeding data.
 * ----------------------------
 */
struct OpMsg {
    struct DocumentSequence {
        std::string name;
        std::vector<BSONObj> objs;
    };

    // Flags
    static constexpr uint32_t kChecksumPresent = 1 << 0;
    static constexpr uint32_t kMoreToCome = 1 << 1;
    static constexpr uint32_t kExhaustSupported = 1 << 16;

    /**
     * Returns the unvalidated flags for the given message if it is an OP_MSG message.
     * Returns 0 for other message kinds since they are the equivalent of no flags set.
     * Throws if the message is too small to hold flags.
     */
    static uint32_t flags(const Message& message);
    static bool isFlagSet(const Message& message, uint32_t flag) {
        return flags(message) & flag;
    }

    /**
     * Replaces the flags in message with the supplied flags.
     * Only legal on an otherwise valid OP_MSG message.
     */
    static void replaceFlags(Message* message, uint32_t flags);

    /**
     * Adds flag to the list of set flags in message.
     * Only legal on an otherwise valid OP_MSG message.
     */
    static void setFlag(Message* message, uint32_t flag) {
        replaceFlags(message, flags(*message) | flag);
    }

    /**
     * Removes a flag from the list of set flags in message.
     * Only legal on an otherwise valid OP_MSG message.
     */
    static void clearFlag(Message* message, uint32_t flag) {
        replaceFlags(message, flags(*message) & ~flag);
    }

    /**
     * Retrieves the checksum stored at the end of the message.
     */
    static uint32_t getChecksum(const Message& message);

    /**
     * Add a checksum at the end of the message. Call this after setting size, requestId, and
     * responseTo. The checksumPresent flag must *not* already be set.
     */
    static void appendChecksum(Message* message);

    /**
     * If the checksum is present, unsets the checksumPresent flag and shrinks message by 4 bytes.
     */
    static void removeChecksum(Message* message) {
        if (!isFlagSet(*message, kChecksumPresent)) {
            return;
        }

        clearFlag(message, kChecksumPresent);
        message->header().setLen(message->size() - 4);
    }

    /**
     * Parses and returns an OpMsg containing unowned BSON.
     */
    static OpMsg parse(const Message& message, Client* client = nullptr);

    /**
     * Parses and returns an OpMsg containing owned BSON.
     */
    static OpMsg parseOwned(const Message& message, Client* client = nullptr) {
        auto msg = parse(message, client);
        msg.shareOwnershipWith(message.sharedBuffer());
        return msg;
    }

    Message serialize() const;

    /**
     * Like serialize() but doesn't enforce max BSON size limits. This is almost never what you
     * want. Prefer serialize() unless there's a good reason to skip the size check.
     */
    Message serializeWithoutSizeChecking() const;

    /**
     * Makes all BSONObjs in this object share ownership with buffer.
     */
    void shareOwnershipWith(const ConstSharedBuffer& buffer);

    /**
     * Returns a pointer to the sequence with the given name or nullptr if there are none.
     */
    const DocumentSequence* getSequence(StringData name) const {
        // Getting N sequences is technically O(N**2) but because there currently is at most 2
        // sequences, this does either 1 or 2 comparisons. Consider making sequences a StringMap if
        // there will be many sequences. This problem may also just go away with the IDL project.
        auto it = std::find_if(
            sequences.begin(), sequences.end(), [&](const auto& seq) { return seq.name == name; });
        return it == sequences.end() ? nullptr : &*it;
    }

    BSONObj body;
    std::vector<DocumentSequence> sequences;

    boost::optional<auth::ValidatedTenancyScope> validatedTenancyScope = boost::none;

    boost::optional<TenantId> getValidatedTenantId() const {
        if (!validatedTenancyScope) {
            return boost::none;
        }
        return validatedTenancyScope->tenantId();
    }
};

/**
 * An OpMsg that represents a request. This is a separate type from OpMsg only to provide better
 * type-safety along with a place to hang request-specific methods.
 */
struct OpMsgRequest : public OpMsg {
    // TODO in C++17 remove constructors so we can use aggregate initialization.
    OpMsgRequest() = default;
    explicit OpMsgRequest(OpMsg&& generic) : OpMsg(std::move(generic)) {}

    static OpMsgRequest parse(const Message& message, Client* client = nullptr) {
        return OpMsgRequest(OpMsg::parse(message, client));
    }

    static OpMsgRequest parseOwned(const Message& message, Client* client = nullptr) {
        return OpMsgRequest(OpMsg::parseOwned(message, client));
    }

    // There are no valid reasons for which a database name should not be a string, but
    // some autogenerated tests can create invalid entries, and some areas of the codebase
    // are required never to throw an exception; these functions should use this version
    // of the getDatabase API.
    StringData getDatabaseNoThrow() const noexcept {
        if (auto elem = body["$db"]; elem.type() == mongo::String) {
            return elem.valueStringData();
        }
        return ""_sd;
    }

    StringData getDatabase() const {
        if (auto elem = body["$db"])
            return elem.checkAndGetStringData();
        uasserted(40571, "OP_MSG requests require a $db argument");
    }

    DatabaseName getDbName() const;

    StringData getCommandName() const {
        return body.firstElementFieldName();
    }

    void setDollarTenant(const TenantId& tenant);

    SerializationContext getSerializationContext() const;

    // DO NOT ADD MEMBERS!  Since this type is essentially a strong typedef (see the class comment),
    // it should not hold more data than an OpMsg. It should be freely interconvertible with OpMsg
    // without issues like slicing.
};

/**
 * Builds an OP_MSG message in-place in a Message buffer.
 *
 * While the OP_MSG format imposes no ordering of sections, in order to efficiently support our
 * usage patterns, this class requires that all document sequences (if any) are built before the
 * body. This allows repeatedly appending fields to the body until right before it is ready to be
 * sent.
 */
class OpMsgBuilder {
    OpMsgBuilder(const OpMsgBuilder&) = delete;
    OpMsgBuilder& operator=(const OpMsgBuilder&) = delete;

public:
    OpMsgBuilder() {
        skipHeaderAndFlags();
    }

    /**
     * See the documentation for DocSequenceBuilder below.
     */
    class DocSequenceBuilder;
    DocSequenceBuilder beginDocSequence(StringData name);

    /**
     * Returns an empty builder for the body.
     * It is an error to call this if a body has already been begun.  You must destroy or call
     * done() on the returned builder before calling any methods on this object.
     */
    BSONObjBuilder beginBody();
    void setBody(const BSONObj& body) {
        beginBody().appendElements(body);
    }

    /**
     * Returns a builder that can be used to append new fields to the body.
     * It is an error to call this if beginBody() hasn't been called yet. It is an error to append
     * elements with field names that already exist in the body. You must destroy or call done() on
     * the returned builder before calling any methods on this object.
     *
     * TODO decide if it is worth keeping the begin/resume distinction in the public API.
     */
    BSONObjBuilder resumeBody();
    void appendElementsToBody(const BSONObj& body) {
        resumeBody().appendElements(body);
    }

    void setSecurityToken(StringData token);

    /**
     * Finish building and return a Message ready to give to the networking layer for transmission.
     * It is illegal to call any methods on this object after calling this.
     * Can throw BSONObjectTooLarge if the internal buffer has grown too large to be converted
     * to a Message within the BSON size limit.
     */
    Message finish();

    /**
     * Like finish() but doesn't enforce max BSON size limits. This is almost never what you want.
     * Prefer finish() unless there's a good reason to skip the size check.
     */
    Message finishWithoutSizeChecking();

    /**
     * Reset this object to its initial empty state. All previously appended data is lost.
     */
    void reset() {
        invariant(!_openBuilder);

        _buf.reset();
        skipHeaderAndFlags();
        _bodyStart = 0;
        _state = kEmpty;
        _openBuilder = false;
    }

    /**
     * Set to true in tests that need to be able to generate duplicate top-level fields to see how
     * the server handles them. Is false by default, although the check only happens in debug
     * builds.
     */
    static AtomicWord<bool> disableDupeFieldCheck_forTest;

    /**
     * Similar to finish, any calls on this object after are illegal.
     */
    BSONObj releaseBody();

    /**
     * Returns whether or not this builder is already building a body.
     */
    bool isBuildingBody() {
        return _state == kBody;
    }

    /**
     * Reserves and claims the bytes requested in the internal BufBuilder.
     */
    void reserveBytes(const std::size_t bytes) {
        _buf.reserveBytes(bytes);
        _buf.claimReservedBytes(bytes);
    }

private:
    friend class DocSequenceBuilder;

    enum State {
        kEmpty,
        kDocSequence,
        kBody,
        kDone,
    };

    void finishDocumentStream(DocSequenceBuilder* docSequenceBuilder);

    void skipHeaderAndFlags() {
        _buf.skip(sizeof(MSGHEADER::Layout));  // This is filled in by finish().
        _buf.appendNum(uint32_t(0));           // flags (currently always 0).
    }

    // When adding members, remember to update reset().
    BufBuilder _buf;
    int _bodyStart = 0;
    State _state = kEmpty;
    bool _openBuilder = false;
};

/**
 * Builds a document sequence in an OpMsgBuilder.
 *
 * Example:
 *
 * auto docSeq = msgBuilder.beginDocSequence("some.sequence");
 *
 * docSeq.append(BSON("a" << 1)); // Copy an obj into the sequence
 *
 * auto bob = docSeq.appendBuilder(); // Build an obj in-place
 * bob.append("a", 2);
 * bob.doneFast();
 *
 * docSeq.done(); // Or just let it go out of scope.
 */
class OpMsgBuilder::DocSequenceBuilder {
    DocSequenceBuilder(const DocSequenceBuilder&) = delete;
    DocSequenceBuilder& operator=(const DocSequenceBuilder&) = delete;

public:
    DocSequenceBuilder(DocSequenceBuilder&& other)
        : _buf(other._buf), _msgBuilder(other._msgBuilder), _sizeOffset(other._sizeOffset) {
        other._buf = nullptr;
    }

    ~DocSequenceBuilder() {
        if (_buf)
            done();
    }

    /**
     * Indicates that the caller is done with this stream prior to destruction.
     * Following this call, it is illegal to call any methods on this object.
     */
    void done() {
        invariant(_buf);
        _msgBuilder->finishDocumentStream(this);
        _buf = nullptr;
    }

    /**
     * Appends a single document to this sequence.
     */
    void append(const BSONObj& obj) {
        _buf->appendBuf(obj.objdata(), obj.objsize());
    }

    /**
     * Returns a BSONObjBuilder that appends a single document to this sequence in place.
     * It is illegal to call any methods on this DocSequenceBuilder until the returned builder
     * is destroyed or done()/doneFast() is called on it.
     */
    BSONObjBuilder appendBuilder() {
        return BSONObjBuilder(*_buf);
    }

    int len() const {
        return _buf->len();
    }

private:
    friend OpMsgBuilder;

    DocSequenceBuilder(OpMsgBuilder* msgBuilder, BufBuilder* buf, int sizeOffset)
        : _buf(buf), _msgBuilder(msgBuilder), _sizeOffset(sizeOffset) {}

    BufBuilder* _buf;
    OpMsgBuilder* const _msgBuilder;
    const int _sizeOffset;
};

/**
 * Builds an OpMsgRequest object.
 */
struct OpMsgRequestBuilder {
public:
    /**
     * Creates an OpMsgRequest object and directly sets a validated tenancy scope on it.
     */
    static OpMsgRequest create(boost::optional<auth::ValidatedTenancyScope> validatedTenancyScope,
                               const DatabaseName& dbName,
                               BSONObj body,
                               const BSONObj& extraFields = {});
};

}  // namespace mongo
