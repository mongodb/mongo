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


#include <bitset>
#include <boost/cstdint.hpp>
#include <fmt/format.h>
#include <memory>
#include <set>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_type_validated.h"
#include "mongo/base/data_view.h"
#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/object_check.h"  // IWYU pragma: keep
#include "mongo/rpc/op_msg.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/str.h"

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
#include <wiredtiger.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {

auto kAllSupportedFlags = OpMsg::kChecksumPresent | OpMsg::kMoreToCome;

bool containsUnknownRequiredFlags(uint32_t flags) {
    const uint32_t kRequiredFlagMask = 0xffff;  // Low 2 bytes are required, high 2 are optional.
    return (flags & ~kAllSupportedFlags & kRequiredFlagMask) != 0;
}

enum class Section : uint8_t {
    kBody = 0,
    kDocSequence = 1,
    kSecurityToken = 2,
};

constexpr int kCrc32Size = 4;

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
// All fields including size, requestId, and responseTo must already be set. The size must
// already include the final 4-byte checksum.
uint32_t calculateChecksum(const Message& message) {
    if (message.operation() != dbMsg) {
        return 0;
    }

    invariant(OpMsg::isFlagSet(message, OpMsg::kChecksumPresent));
    return wiredtiger_crc32c_func()(message.singleData().view2ptr(), message.size() - kCrc32Size);
}
#endif  // MONGO_CONFIG_WIREDTIGER_ENABLED
}  // namespace

uint32_t OpMsg::flags(const Message& message) {
    if (message.operation() != dbMsg)
        return 0;  // Other command protocols are the same as no flags set.

    return BufReader(message.singleData().data(), message.dataSize())
        .read<LittleEndian<uint32_t>>();
}

void OpMsg::replaceFlags(Message* message, uint32_t flags) {
    invariant(!message->empty());
    invariant(message->operation() == dbMsg);
    invariant(message->dataSize() >= static_cast<int>(sizeof(uint32_t)));

    DataView(message->singleData().data()).write<LittleEndian<uint32_t>>(flags);
}

uint32_t OpMsg::getChecksum(const Message& message) {
    invariant(message.operation() == dbMsg);
    invariant(isFlagSet(message, kChecksumPresent));
    uassert(51252,
            "Invalid message size for an OpMsg containing a checksum",
            // Check that the message size is at least the size of a crc-32 checksum and
            // the 32-bit flags section.
            message.dataSize() > static_cast<int>(kCrc32Size + sizeof(uint32_t)));
    return BufReader(message.singleData().view2ptr() + message.size() - kCrc32Size, kCrc32Size)
        .read<LittleEndian<uint32_t>>();
}

void OpMsg::appendChecksum(Message* message) {
#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
    if (message->operation() != dbMsg) {
        return;
    }

    invariant(!isFlagSet(*message, kChecksumPresent));
    setFlag(message, kChecksumPresent);
    const size_t newSize = message->size() + kCrc32Size;
    if (message->capacity() < newSize) {
        message->realloc(newSize);
    }

    // Everything before the checksum, including the final size, is covered by the checksum.
    message->header().setLen(newSize);
    DataView(message->singleData().view2ptr() + newSize - kCrc32Size)
        .write<LittleEndian<uint32_t>>(calculateChecksum(*message));
#endif
}

OpMsg OpMsg::parse(const Message& message, Client* client) try {
    // It is the caller's responsibility to call the correct parser for a given message type.
    invariant(!message.empty());
    invariant(message.operation() == dbMsg);

    const uint32_t flags = OpMsg::flags(message);
    uassert(ErrorCodes::IllegalOpMsgFlag,
            str::stream() << "Message contains illegal flags value: Ob"
                          << std::bitset<32>(flags).to_string(),
            !containsUnknownRequiredFlags(flags));

    auto dataSize = message.dataSize() - sizeof(flags);
    boost::optional<uint32_t> checksum;
    if (flags & kChecksumPresent) {
        checksum = getChecksum(message);
        uassert(51251,
                "Invalid message size for an OpMsg containing a checksum",
                dataSize > kCrc32Size);
        dataSize -= kCrc32Size;
    }

    // The sections begin after the flags and before the checksum (if present).
    BufReader sectionsBuf(message.singleData().data() + sizeof(flags), dataSize);

    // TODO some validation may make more sense in the IDL parser. I've tagged them with
    // comments.
    bool haveBody = false;
    OpMsg msg;
    StringData securityToken;
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
                // We use an O(N^2) algorithm here and an O(N*M) algorithm below. These are
                // fastest for the current small values of N, but would be problematic if it is
                // large. If we need more document sequences, raise the limit and use a better
                // algorithm.
                uassert(ErrorCodes::TooManyDocumentSequences,
                        "Too many document sequences in OP_MSG",
                        msg.sequences.size() < 2);  // Limit is <=2 since we are about to add one.

                // The first 4 bytes are the total size, including themselves.
                const auto remainingSize =
                    sectionsBuf.read<LittleEndian<int32_t>>() - sizeof(int32_t);
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

            case Section::kSecurityToken: {
                // if op_msg is parsed by mongoBridge, bridge has a backup check since multitenancy
                // should be false
                uassert(ErrorCodes::Unauthorized,
                        "Unsupported Security Token provided",
                        gMultitenancySupport || serverGlobalParams.isMongoBridge);
                securityToken = sectionsBuf.readCStr();
                break;
            }

            default:
                // Using uint32_t so we append as a decimal number rather than as a char.
                uasserted(40432, str::stream() << "Unknown section kind " << uint32_t(sectionKind));
        }
    }

    uassert(40587, "OP_MSG messages must have a body", haveBody);

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

#ifdef MONGO_CONFIG_WIREDTIGER_ENABLED
    if (checksum) {
        uassert(ErrorCodes::ChecksumMismatch,
                "OP_MSG checksum does not match contents",
                *checksum == calculateChecksum(message));
    }
#endif
    if (gMultitenancySupport) {
        msg.validatedTenancyScope =
            auth::ValidatedTenancyScopeFactory::parse(client, msg.body, securityToken);
    }

    return msg;
} catch (const DBException& ex) {
    LOGV2_DEBUG(
        22632,
        1,
        "invalid message: {ex_code} {ex} -- {hexdump_message_singleData_view2ptr_message_size}",
        "ex_code"_attr = ex.code(),
        "ex"_attr = redact(ex),
        // Using std::min to reduce the size of the output and ensure we do not throw in hexdump()
        // because of the exceeded length.
        "hexdump_message_singleData_view2ptr_message_size"_attr =
            redact(hexdump(message.singleData().view2ptr(),
                           std::min(static_cast<size_t>(message.size()), kHexDumpMaxSize - 1))));
    throw;
}

boost::optional<TenantId> parseDollarTenant(const BSONObj body) {
    if (auto tenant = body.getField("$tenant")) {
        return TenantId::parseFromBSON(tenant);
    } else {
        return boost::none;
    }
}

DatabaseName OpMsgRequest::getDbName() const {
    if (!gMultitenancySupport) {
        return DatabaseNameUtil::deserialize(boost::none, getDatabase(), getSerializationContext());
    }
    auto tenantId = getValidatedTenantId() ? getValidatedTenantId() : parseDollarTenant(body);

    return DatabaseNameUtil::deserialize(tenantId, getDatabase(), getSerializationContext());
}

SerializationContext OpMsgRequest::getSerializationContext() const {
    if (!gMultitenancySupport) {
        return SerializationContext::stateDefault();
    }
    auto serializationCtx = SerializationContext::stateCommandRequest();
    auto tenantId = getValidatedTenantId() ? getValidatedTenantId() : parseDollarTenant(body);

    if (auto const expectPrefix = body.getField("expectPrefix")) {
        serializationCtx.setPrefixState(expectPrefix.boolean());
    } else if (validatedTenancyScope) {
        serializationCtx.setPrefixState(validatedTenancyScope->isFromAtlasProxy());
    }
    return serializationCtx;
}


bool appendDollarTenant(BSONObjBuilder& builder,
                        const TenantId& tenant,
                        boost::optional<TenantId> existingDollarTenant = boost::none) {
    if (existingDollarTenant) {
        massert(8423373,
                str::stream() << "Unable to set TenantId '" << tenant
                              << "' on OpMsgRequest as it already has "
                              << existingDollarTenant->toString(),
                tenant == existingDollarTenant.value());
        return true;
    }

    if (gMultitenancySupport) {
        tenant.serializeToBSON("$tenant", &builder);
        return true;
    }
    return false;
}

BSONObj appendDollarDbAndTenant(const DatabaseName& dbName,
                                BSONObj body,
                                const SerializationContext& sc,
                                const bool hasVts,
                                const BSONObj& extraFields = {}) {
    auto existingDollarTenant = parseDollarTenant(body);
    BSONObjBuilder builder(std::move(body));
    builder.appendElements(extraFields);

    // Recreate each of the fields according to the sc when copying the body from an existing
    // request.
    if (!hasVts && dbName.tenantId()) {
        appendDollarTenant(builder, dbName.tenantId().value(), existingDollarTenant);
    }
    builder.append("$db", DatabaseNameUtil::serialize(dbName, sc));
    if (sc.getPrefix() == SerializationContext::Prefix::IncludePrefix) {
        builder.append("expectPrefix",
                       sc.getPrefix() == SerializationContext::Prefix::IncludePrefix);
    }

    return builder.obj();
}

void OpMsgRequest::setDollarTenant(const TenantId& tenant) {
    massert(8423372,
            str::stream() << "Should not set dollar tenant " << tenant
                          << " on the validated OpMsgRequest.",
            !validatedTenancyScope);

    auto dollarTenant = parseDollarTenant(body);
    BSONObjBuilder bodyBuilder(std::move(body));
    appendDollarTenant(bodyBuilder, tenant, dollarTenant);
    body = bodyBuilder.obj();
}

namespace {
std::string compactStr(const std::string& input) {
    if (input.length() > 2024) {
        return input.substr(0, 1000) + " ... " + input.substr(input.length() - 1000);
    }
    return input;
}
}  // namespace
void validateExtraFields(const DatabaseName& dbName,
                         const BSONObj& body,
                         const BSONObj& extraFields) {
    int bodySize = body.objsize();
    int extraFieldsSize = extraFields.objsize();

    // Log a warning if the sum of the sizes of 'body' and 'extraFields' exceeds
    // 'BSONObjMaxInternalSize'.
    if (bodySize + extraFieldsSize > BSONObjMaxInternalSize) {
        LOGV2_WARNING(
            6491800,
            "Request body exceeded limit with body.objsize() = {bodySize} bytes, "
            "extraFields.objsize() = {extraFieldsSize} bytes, body.toString() = {body}, db = "
            "{db}, extraFields.toString() = {extraFields}",
            "bodySize"_attr = bodySize,
            "extraFieldsSize"_attr = extraFieldsSize,
            "body"_attr = compactStr(body.toString()),
            "db"_attr = dbName.toStringForErrorMsg(),
            "extraFields"_attr = compactStr(extraFields.toString()));
    };
}

OpMsgRequest OpMsgRequestBuilder::create(
    boost::optional<auth::ValidatedTenancyScope> validatedTenancyScope,
    const DatabaseName& dbName,
    BSONObj body,
    const BSONObj& extraFields) {
    validateExtraFields(dbName, body, extraFields);

    const SerializationContext sc = validatedTenancyScope != boost::none
        ? SerializationContext::stateCommandRequest(validatedTenancyScope->hasTenantId(),
                                                    validatedTenancyScope->isFromAtlasProxy())
        : SerializationContext::stateCommandRequest();

    OpMsgRequest request;
    const bool hasValidVts = validatedTenancyScope && validatedTenancyScope->isValid();
    request.body = appendDollarDbAndTenant(dbName, std::move(body), sc, hasValidVts, extraFields);
    if (hasValidVts) {
        request.validatedTenancyScope = std::move(validatedTenancyScope);
    }

    return request;
}

namespace {
void serializeHelper(const std::vector<OpMsg::DocumentSequence>& sequences,
                     const BSONObj& body,
                     const boost::optional<auth::ValidatedTenancyScope>& validatedTenancyScope,
                     OpMsgBuilder* output) {
    if (validatedTenancyScope) {
        auto securityToken = validatedTenancyScope->getOriginalToken();
        if (!securityToken.empty()) {
            output->setSecurityToken(securityToken);
        }
    }
    for (auto&& seq : sequences) {
        auto docSeq = output->beginDocSequence(seq.name);
        for (auto&& obj : seq.objs) {
            docSeq.append(obj);
        }
    }
    output->beginBody().appendElements(body);
}
}  // namespace

Message OpMsg::serialize() const {
    OpMsgBuilder builder;
    serializeHelper(sequences, body, validatedTenancyScope, &builder);
    return builder.finish();
}

Message OpMsg::serializeWithoutSizeChecking() const {
    OpMsgBuilder builder;
    serializeHelper(sequences, body, validatedTenancyScope, &builder);
    return builder.finishWithoutSizeChecking();
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

void OpMsgBuilder::setSecurityToken(StringData token) {
    invariant(_state == kEmpty);
    _buf.appendStruct(Section::kSecurityToken);
    _buf.appendStr(token, true /* includeEndingNull */);
}

auto OpMsgBuilder::beginDocSequence(StringData name) -> DocSequenceBuilder {
    invariant((_state == kEmpty) || (_state == kDocSequence));
    invariant(!_openBuilder);
    _openBuilder = true;
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
    invariant((_state == kEmpty) || (_state == kDocSequence));
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

AtomicWord<bool> OpMsgBuilder::disableDupeFieldCheck_forTest{false};

Message OpMsgBuilder::finish() {
    const auto size = _buf.len();
    uassert(ErrorCodes::BSONObjectTooLarge,
            str::stream() << "BSON size limit hit while building Message. Size: " << size << " (0x"
                          << unsignedHex(size) << "); maxSize: " << BSONObjMaxInternalSize << "("
                          << (BSONObjMaxInternalSize / (1024 * 1024)) << "MB)",
            size <= BSONObjMaxInternalSize);

    return finishWithoutSizeChecking();
}

Message OpMsgBuilder::finishWithoutSizeChecking() {
    if (kDebugBuild && !disableDupeFieldCheck_forTest.load()) {
        std::set<StringData> seenFields;
        for (auto elem : resumeBody().asTempObj()) {
            if (!(seenFields.insert(elem.fieldNameStringData()).second)) {
                LOGV2_FATAL(40474,
                            "OP_MSG with duplicate field '{elem_fieldNameStringData}' : "
                            "{resumeBody_asTempObj}",
                            "elem_fieldNameStringData"_attr = elem.fieldNameStringData(),
                            "resumeBody_asTempObj"_attr = redact(resumeBody().asTempObj()));
            }
        }
    }

    invariant(_state == kBody);
    invariant(_bodyStart);
    invariant(!_openBuilder);
    _state = kDone;

    const auto size = _buf.len();
    MSGHEADER::View header(_buf.buf());
    header.setMessageLength(size);
    // header.setRequestMsgId(...); // These are currently filled in by the networking layer.
    // header.setResponseToMsgId(...);
    header.setOpCode(dbMsg);
    return Message(_buf.release());
}

BSONObj OpMsgBuilder::releaseBody() {
    invariant(_state == kBody);
    invariant(_bodyStart);
    invariant(_bodyStart == sizeof(MSGHEADER::Layout) + 4 /*flags*/ + 1 /*body kind byte*/);
    invariant(!_openBuilder);
    _state = kDone;

    auto bson = BSONObj(_buf.buf() + _bodyStart);
    return bson.shareOwnershipWith(_buf.release());
}

}  // namespace mongo
