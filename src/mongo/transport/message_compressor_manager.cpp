// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/transport/message_compressor_manager.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_compressed.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/shared_buffer.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace {
const transport::Session::Decoration<MessageCompressorManager> getForSession =
    transport::Session::declareDecoration<MessageCompressorManager>();
}  // namespace

MessageCompressorManager::MessageCompressorManager()
    : MessageCompressorManager(&MessageCompressorRegistry::get()) {}

MessageCompressorManager::MessageCompressorManager(MessageCompressorRegistry* factory)
    : _registry{factory} {}

StatusWith<Message> MessageCompressorManager::compressMessage(
    const Message& msg, const MessageCompressorId* compressorId) {

    MessageCompressorBase* compressor = nullptr;
    if (compressorId) {
        compressor = _registry->getCompressor(*compressorId);
        invariant(compressor);
        // an explicit id is normally the echo-back id captured by
        // decompressMessage(). Since the registry is now the net/repl union, a registered compressor
        // may still be outside this connection's policy. Do not emit unpermitted compressors; legacy
        // callers with no permit list keep the old behavior.
        if (!_isCompressorPermittedForThisSession(*compressorId)) {
            LOGV2_DEBUG(10130418,
                        3,
                        "Compressor not permitted for this connection; sending uncompressed",
                        "compressorId"_attr = static_cast<int>(*compressorId));
            return {msg};
        }
    } else if (!_negotiated.empty()) {
        compressor = _negotiated[0];
    } else {
        return {msg};
    }

    LOGV2_DEBUG(22925, 3, "Compressing message", "compressor"_attr = compressor->getName());

    auto inputHeader = msg.header();
    size_t bufferSize = compressor->getMaxCompressedSize(msg.dataSize()) +
        CompressionHeader::size() + MsgData::MsgDataHeaderSize;

    CompressionHeader compressionHeader(
        inputHeader.getNetworkOp(), inputHeader.dataLen(), compressor->getId());

    if (bufferSize > MaxMessageSizeBytes) {
        LOGV2_DEBUG(22926,
                    3,
                    "Compressed message would be larger than maximum allowed, returning original "
                    "uncompressed message",
                    "MaxMessageSizeBytes"_attr = MaxMessageSizeBytes);
        return {msg};
    }

    auto outputMessageBuffer = SharedBuffer::allocate(bufferSize);

    MsgData::View outMessage(outputMessageBuffer.get());
    outMessage.setId(inputHeader.getId());
    outMessage.setResponseToMsgId(inputHeader.getResponseToMsgId());
    outMessage.setOperation(dbCompressed);
    outMessage.setLen(bufferSize);

    DataRangeCursor output(outMessage.data(), outMessage.data() + outMessage.dataLen());
    compressionHeader.serialize(&output);
    ConstDataRange input(inputHeader.data(), inputHeader.data() + inputHeader.dataLen());

    auto sws = compressor->compressData(input, output);

    if (!sws.isOK())
        return sws.getStatus();

    auto realCompressedSize = sws.getValue();
    outMessage.setLen(realCompressedSize + CompressionHeader::size() + MsgData::MsgDataHeaderSize);

    // Additionally attribute this message to the replication-specific counters when this connection
    // is marked as replication data-plane traffic. This is a subset of the process-wide counters the
    // compressor already bumped inside compressData(), so serverStatus().network.compression is
    // unchanged; serverStatus().repl.compression reports just the replication portion.
    if (_countAsReplicationCompressionTraffic) {
        compressor->counterHitReplicationCompress(input.length(), realCompressedSize);
    }

    return {Message(outputMessageBuffer)};
}

StatusWith<Message> MessageCompressorManager::decompressMessage(const Message& msg,
                                                                MessageCompressorId* compressorId,
                                                                size_t maxMessageSize) {
    auto inputHeader = msg.header();
    ConstDataRangeCursor input(inputHeader.data(), inputHeader.data() + inputHeader.dataLen());
    if (input.length() < CompressionHeader::size()) {
        return {ErrorCodes::BadValue, "Invalid compressed message header"};
    }
    CompressionHeader compressionHeader(&input);

    auto compressor = _registry->getCompressor(compressionHeader.compressorId);
    if (!compressor) {
        return {ErrorCodes::InternalError,
                "Compression algorithm specified in message is not available"};
    }

    // registry membership now means process capability, not per-connection policy.
    // Once a permit list is engaged, reject compressed frames whose compressor was not permitted for
    // this connection - otherwise an external connection could decompress a replication-only frame and
    // net.compression.compressors: disabled would not truly disable it. Legacy callers without a permit
    // list keep the old registry-wide behavior.
    if (!_isCompressorPermittedForThisSession(compressionHeader.compressorId)) {
        return {ErrorCodes::BadValue, "Compressor was not negotiated for this connection"};
    }

    if (compressorId) {
        *compressorId = compressor->getId();
    }

    LOGV2_DEBUG(22927, 3, "Decompressing message", "compressor"_attr = compressor->getName());

    if (compressionHeader.uncompressedSize < 0) {
        return {ErrorCodes::BadValue, "Decompressed message would be negative in size"};
    }

    // Explicitly promote `uncompressedSize` to a 64-bit integer before addition in order to
    // avoid potential overflow.
    size_t bufferSize =
        static_cast<size_t>(compressionHeader.uncompressedSize) + MsgData::MsgDataHeaderSize;
    if (bufferSize > maxMessageSize) {
        return {ErrorCodes::BadValue,
                "Decompressed message would be larger than maximum message size"};
    }

    auto maxDecompressedSize = compressor->getMaxDecompressedSize(input);
    if (maxDecompressedSize &&
        *maxDecompressedSize < static_cast<std::size_t>(compressionHeader.uncompressedSize)) {
        return {ErrorCodes::BadValue, "Uncompressed message size does not match expected size"};
    }

    auto outputMessageBuffer = SharedBuffer::allocate(bufferSize);
    MsgData::View outMessage(outputMessageBuffer.get());
    outMessage.setId(inputHeader.getId());
    outMessage.setResponseToMsgId(inputHeader.getResponseToMsgId());
    outMessage.setOperation(compressionHeader.originalOpCode);
    outMessage.setLen(bufferSize);

    DataRangeCursor output(outMessage.data(), outMessage.data() + outMessage.dataLen());

    auto sws = compressor->decompressData(input, output);

    if (!sws.isOK())
        return sws.getStatus();

    if (sws.getValue() != static_cast<std::size_t>(compressionHeader.uncompressedSize)) {
        return {ErrorCodes::BadValue, "Decompressing message returned less data than expected"};
    }

    outMessage.setLen(sws.getValue() + MsgData::MsgDataHeaderSize);

    // Additionally attribute this message to the replication-specific counters when this connection
    // is marked as replication data-plane traffic. This is a subset of the process-wide counters
    // that decompressData() already bumped, so serverStatus().network.compression is unchanged.
    if (_countAsReplicationCompressionTraffic) {
        compressor->counterHitReplicationDecompress(input.length(), sws.getValue());
    }

    return {Message(outputMessageBuffer)};
}

void MessageCompressorManager::clientBegin(BSONObjBuilder* output) {
    LOGV2_DEBUG(22928, 3, "Starting client-side compression negotiation");

    // We're about to update the compressor list with the negotiation result from the server.
    _negotiated.clear();
    _advertisedCompressors.clear();
    // Until clientFinish() accepts the server's hello response, this client connection has not
    // negotiated any inbound compressed response. Engage an empty permit list now so a malformed
    // peer cannot send OP_COMPRESSED during the handshake and have it decoded via the process-wide
    // union registry.
    _permittedCompressorIds.emplace();
    _permittedCompressorIds->fill(false);

    // tag replication data-plane connections (oplog fetcher, initial-sync cloner,
    // rollback remote oplog reader) so the server applies the replication compression policy to this
    // connection only. Emit the marker even when compression is suppressed or inherited, so other
    // internal connections (heartbeats, shard RPC) continue to negotiate against net.compression.
    if (_isReplicationClient) {
        output->append(kReplicationCompressionClientFieldName, true);
    }

    // per-session opt-out used for replicationNetworkCompression: "disabled".
    // Omit the "compression" array from hello so this connection negotiates uncompressed, without
    // changing the process-wide net.compression.compressors policy or any other connection.
    if (!_offerCompressionOnClientBegin) {
        LOGV2_DEBUG(
            10130410,
            3,
            "Compression advertisement suppressed for this connection; negotiating uncompressed");
        return;
    }

    // replication data-plane clients may provide a per-session compressor allow-list.
    // Advertise its intersection with the process-wide net/repl capability union. After normal
    // startup finalization this is expected to be a no-op filter, but it keeps tests/direct callers
    // from advertising compressors the process did not register. Preserve caller order as negotiation
    // preference; an empty intersection means this connection negotiates uncompressed.
    if (!_allowListedCompressors.empty()) {
        const auto& compressorList = _registry->getCompressorNames();
        std::vector<std::string_view> filtered;
        filtered.reserve(_allowListedCompressors.size());
        for (const auto& name : _allowListedCompressors) {
            const auto it = std::find(compressorList.begin(), compressorList.end(), name);
            if (it == compressorList.end()) {
                LOGV2_WARNING(10130411,
                              "Ignoring per-session compressor not registered process-wide",
                              "compressor"_attr = name);
                continue;
            }
            filtered.push_back(*it);
        }
        if (filtered.empty()) {
            LOGV2_WARNING(10130412,
                          "Requested per-session compressors are not registered process-wide; this "
                          "connection will be uncompressed",
                          "requestedCompressors"_attr = _allowListedCompressors);
            return;
        }
        BSONArrayBuilder sub(output->subarrayStart("compression"));
        for (const auto& e : filtered) {
            LOGV2_DEBUG(10130413,
                        3,
                        "Offering compressor to server (per-session allow-list)",
                        "compressor"_attr = e);
            sub.append(e);
            _advertisedCompressors.emplace_back(e);
        }
        sub.doneFast();
        return;
    }

    // Default path: advertise only the client-facing net set (net.compression.compressors). A
    // connection without a per-session allow-list is treated as external, so it must not offer
    // replication-only compressors. When net.compression.compressors: disabled this set is empty
    // and nothing is advertised.
    const auto& netCompressorList = _registry->getNetCompressorNames();
    if (netCompressorList.empty())
        return;

    BSONArrayBuilder sub(output->subarrayStart("compression"));
    for (const auto& e : netCompressorList) {
        LOGV2_DEBUG(22929, 3, "Offering compressor to server", "compressor"_attr = e);
        sub.append(e);
        _advertisedCompressors.push_back(e);
    }
    sub.doneFast();
}

void MessageCompressorManager::clientFinish(const BSONObj& input) {
    auto elem = input.getField("compression");
    LOGV2_DEBUG(22930, 3, "Finishing client-side compression negotiation");

    // We've just called clientBegin, so the list of compressors should be empty.
    invariant(_negotiated.empty());

    // Start from an empty inbound permit list. Only compressors accepted from the server's hello
    // response below become legal for subsequent OP_COMPRESSED responses on this client connection.
    _permittedCompressorIds.emplace();
    auto& permitted = *_permittedCompressorIds;
    permitted.fill(false);

    auto logReplicationNegotiationResult = [&] {
        if (!_isReplicationClient) {
            return;
        }
        std::vector<std::string> negotiatedNames;
        negotiatedNames.reserve(_negotiated.size());
        for (const auto* compressor : _negotiated) {
            negotiatedNames.emplace_back(compressor->getName());
        }
        LOGV2(10130422,
              "Replication network compression negotiation completed",
              "compressed"_attr = !_negotiated.empty(),
              "negotiatedCompressors"_attr = negotiatedNames);
    };

    // If the server didn't send back a "compression" array, then we assume compression is not
    // supported by this server and just return. We've already disabled compression by clearing
    // out the _negotiated array above, and the empty permit list rejects any compressed response.
    if (elem.eoo()) {
        LOGV2_DEBUG(22931,
                    3,
                    "No compression algorithms were sent from the server. This connection will be "
                    "uncompressed");
        logReplicationNegotiationResult();
        return;
    }

    LOGV2_DEBUG(22932, 3, "Received message compressors from server");
    for (const auto& e : elem.Obj()) {
        std::string algoName{e.checkAndGetStringData()};

        // Only accept compressors this client actually advertised in clientBegin(). A malformed
        // peer could otherwise override this connection's local compression policy, including
        // disabled compression or a per-session allow-list.
        const bool advertised = std::find(_advertisedCompressors.begin(),
                                          _advertisedCompressors.end(),
                                          algoName) != _advertisedCompressors.end();
        if (!advertised) {
            LOGV2_DEBUG(10130421,
                        3,
                        "Ignoring compressor not advertised by this client in server hello response",
                        "compressor"_attr = algoName);
            continue;
        }

        auto ret = _registry->getCompressor(algoName);
        // Defend against a malformed or malicious server response. getCompressor() returns nullptr
        // for an unknown/unregistered name, and dereferencing it (ret->getName()) would crash the
        // client. Skip unknown names so the connection simply negotiates the remaining understood
        // compressors, or ends up uncompressed if none are understood. (An advertised name is
        // normally registered, so this is a belt-and-suspenders guard.)
        if (!ret) {
            LOGV2_DEBUG(10130419,
                        3,
                        "Ignoring unknown compressor in server hello response",
                        "compressor"_attr = algoName);
            continue;
        }
        LOGV2_DEBUG(22933, 3, "Adding compressor", "compressor"_attr = ret->getName());
        _negotiated.push_back(ret);
        permitted[ret->getId()] = true;
    }
    logReplicationNegotiationResult();
}

void MessageCompressorManager::serverNegotiate(
    const boost::optional<std::vector<std::string_view>>& clientCompressors,
    BSONObjBuilder* result,
    const boost::optional<std::vector<std::string>>& serverCompressorAllowList) {
    LOGV2_DEBUG(22934, 3, "Starting server-side compression negotiation");

    // No advertised compressions, just asking for the last negotiated result.
    if (!clientCompressors) {
        // If we haven't negotiated any compressors yet, then don't append anything to the
        // output - this makes this compatible with older versions of MongoDB that don't
        // support compression.
        std::vector<std::string> ret;
        if (_negotiated.empty()) {
            LOGV2_DEBUG(22935, 3, "Compression negotiation not requested by client");
            // client didn't request compression and nothing was negotiated. Engage an empty
            // permit list so any stray compressed frame is rejected; otherwise the disengaged
            // list falls back to the process-wide net/repl registry and bypasses a disabled net policy.
            _permittedCompressorIds.emplace();
            _permittedCompressorIds->fill(false);
        } else {
            BSONArrayBuilder sub(result->subarrayStart("compression"));
            for (const auto& algo : _negotiated) {
                sub << algo->getName();
            }
        }
        return;
    }

    // If compression has already been negotiated, then this is a renegotiation, so we should
    // reset the state of the manager.
    _negotiated.clear();

    // First we go through all the compressor names that the client has requested support for
    if (clientCompressors->empty()) {
        LOGV2_DEBUG(22936, 3, "No compressors provided");
        // Engage an empty permit list so this server connection rejects any compressed frame; the
        // client asked for no compression.
        _permittedCompressorIds.emplace();
        _permittedCompressorIds->fill(false);
        return;
    }

    const std::vector<std::string>& allowed = serverCompressorAllowList
        ? *serverCompressorAllowList
        : _registry->getNetCompressorNames();

    // build this connection's compressor-id permit list from 'allowed', not from
    // the process-wide net/repl registry. This keeps external connections from using
    // replication-only compressors while preserving in-domain behavior. If 'allowed'
    // is empty, the permit list stays all-false and the connection is uncompressed.
    _permittedCompressorIds.emplace();
    auto& permitted = *_permittedCompressorIds;
    permitted.fill(false);
    for (const auto& name : allowed) {
        if (auto* c = _registry->getCompressor(name)) {
            permitted[c->getId()] = true;
        }
    }

    // Track whether the client advertised a compressor that this connection is permitted to use but
    // that this build has NOT registered process-wide. This is a defensive
    // guard: a compressor named in net.compression.compressors or
    // replication.networkCompression.compressors that is not compiled into this build now fails
    // startup outright (finalizeSupportedCompressors), so on a correctly started node this state
    // should not arise. If it ever does (e.g. a registry inconsistency), the connection silently
    // ends up uncompressed, so we still surface it below rather than fail the negotiation.
    bool droppedPermittedButUnregistered = false;
    for (const auto& curName : *clientCompressors) {
        MessageCompressorBase* cur;
        // Note: named 'isCandidate' rather than 'permitted' to avoid shadowing the
        // '_permittedCompressorIds' array reference bound above (would trip -Wshadow / -Werror).
        const bool isCandidate = std::any_of(
            allowed.begin(), allowed.end(), [&](const std::string& allowedName) {
                return std::string_view(allowedName) == curName;
            });
        if (!isCandidate) {
            LOGV2_DEBUG(10130415,
                        3,
                        "Rejecting compressor not permitted for this connection type",
                        "compressor"_attr = curName);
            continue;
        }
        // If the MessageCompressorRegistry knows about a compressor with that name, then it is
        // valid and we add it to our list of negotiated compressors.
        if ((cur = _registry->getCompressor(curName))) {
            LOGV2_DEBUG(22937, 3, "supported compressor", "compressor"_attr = cur->getName());
            _negotiated.push_back(cur);
        } else {  // Otherwise the compressor is not supported and we skip over it.
            LOGV2_DEBUG(22938, 3, "compressor is not supported", "compressor"_attr = curName);
            droppedPermittedButUnregistered = true;
        }
    }

    // If the number of compressors that were eventually negotiated is greater than 0, then
    // we should send that back to the client.
    if (_negotiated.empty()) {
        // When this is a replication connection (a candidate list was supplied) that
        // permitted at least one advertised algorithm but none of them are registered in this
        // build, the channel is silently uncompressed despite the operator having configured
        // replicationNetworkCompression. Surface it at WARNING (rate-limited, since it recurs on
        // every reconnect) so this is not only visible at DEBUG level 3. With uncompiled algorithms
        // now rejected at startup, reaching this branch indicates an unexpected registry
        // inconsistency rather than an ordinary misconfiguration.
        if (serverCompressorAllowList && droppedPermittedButUnregistered) {
            LOGV2_WARNING(10130417,
                          "A replication connection advertised compressor(s) permitted by "
                          "replicationNetworkCompression, but none are available in this build; the "
                          "connection will be uncompressed. Only compressors present at startup can "
                          "be used",
                          "clientAdvertised"_attr = *clientCompressors,
                          "permittedCandidates"_attr = allowed);
        } else {
            LOGV2_DEBUG(22939, 3, "Could not agree on compressor to use");
        }
    } else {
        BSONArrayBuilder sub(result->subarrayStart("compression"));
        for (const auto& algo : _negotiated) {
            sub << algo->getName();
        }
    }
}

const std::vector<MessageCompressorBase*>& MessageCompressorManager::getNegotiatedCompressors()
    const {
    return _negotiated;
}

MessageCompressorManager& MessageCompressorManager::forSession(
    const std::shared_ptr<transport::Session>& session) {
    return getForSession(session.get());
}

}  // namespace mongo
