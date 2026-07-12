// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/session.h"
#include "mongo/util/modules.h"

#include <array>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Message;

class MessageCompressorRegistry;

class [[MONGO_MOD_PUBLIC]] MessageCompressorManager {
    MessageCompressorManager(const MessageCompressorManager&) = delete;
    MessageCompressorManager& operator=(const MessageCompressorManager&) = delete;

    // Forward-declared members used by inline methods below. Full private section
    // with initializers appears at the end of the class.
    bool _offerCompressionOnClientBegin{true};
    std::vector<std::string> _allowListedCompressors;
    // True for replication data-plane client connections (oplog fetcher, initial sync, rollback).
    // When set, clientBegin() adds the replicationCompressionClient marker so the server applies
    // the replication compression policy; other internal clients keep using net.compression.
    bool _isReplicationClient{false};

    // true when compression bytes on this connection should be attributed to
    // serverStatus().repl.compression. This is intentionally independent from _isReplicationClient:
    // client-side replication data-plane connections need both (send marker + count bytes), while
    // the server side of a marked replication connection only needs the accounting flag.
    bool _countAsReplicationCompressionTraffic{false};

public:
    /*
     * Default constructor. Uses the global MessageCompressorRegistry.
     */
    MessageCompressorManager();

    /*
     * Constructs a manager from a specific MessageCompressorRegistry - used by the unit tests
     * to test various registry configurations.
     */
    explicit MessageCompressorManager(MessageCompressorRegistry* factory);

    MessageCompressorManager(MessageCompressorManager&&) = default;
    MessageCompressorManager& operator=(MessageCompressorManager&&) = default;

    /*
     * Called by a client constructing a "hello" request. By default, advertises the client-facing
     * net.compression compressors. A per-session allow-list restricts what is advertised for
     * replication data-plane connections; disableCompressionForThisSession() advertises nothing.
     */
    void clientBegin(BSONObjBuilder* output);

    /*
     * Suppresses compressor advertisement on the next (and subsequent) clientBegin() calls made
     * on this manager. This is a per-session, client-side switch: it has no effect on the
     * server-side negotiation of any other connection. A connection that opted out simply won't
     * have negotiated any compressor, so compressMessage() returns the original message unchanged
     * and decompressMessage() rejects any compressed response once the handshake completes.
     *
     * Must be called before the "hello" handshake is sent on the underlying connection. Once set,
     * the suppression persists across DBClientConnection auto-reconnects because the manager
     * instance is reused.
     */
    void disableCompressionForThisSession() {
        _offerCompressionOnClientBegin = false;
    }

    /*
     * Re-enables compressor advertisement on the next clientBegin() call. Provided so that a
     * manager instance can be flipped back to normal behavior if the caller later decides to
     * offer compression on subsequent handshakes; the change only takes effect on the next
     * handshake, not on an already-negotiated connection.
     */
    void enableCompressionForThisSession() {
        _offerCompressionOnClientBegin = true;
    }

    bool isCompressionOfferedForThisSession() const {
        return _offerCompressionOnClientBegin;
    }

    /*
     * Restricts compressors advertised by clientBegin() to this per-session allow-list,
     * intersected with the process-wide net/repl capability set. This lets replication data-plane
     * clients advertise replication-only compressors; passing an empty vector returns to the net set.
     */
    void setCompressorAllowListForThisSession(std::vector<std::string> allowList) {
        _allowListedCompressors = std::move(allowList);
    }

    const std::vector<std::string>& getCompressorAllowListForThisSession() const {
        return _allowListedCompressors;
    }

    /*
     * Hello marker used by replication data-plane clients so the server applies the replication
     * compression policy instead of the normal net.compression policy. Older servers ignore the
     * unknown field because hello is non-strict.
     */
    static constexpr auto kReplicationCompressionClientFieldName = "replicationCompressionClient";

    /*
     * Marks this manager as a replication data-plane client. clientBegin() will add
     * kReplicationCompressionClientFieldName to hello; the flag persists across reconnects because
     * the manager instance is reused.
     */
    void markReplicationClientForThisSession(bool isReplicationClient) {
        _isReplicationClient = isReplicationClient;
    }

    bool isReplicationClientForThisSession() const {
        return _isReplicationClient;
    }

    /*
     * Marks whether this connection's compression bytes count toward serverStatus().repl.compression.
     * Kept separate from markReplicationClientForThisSession(): server-side inbound replication
     * connections need accounting without emitting the client hello marker.
     */
    void countAsReplicationCompressionTrafficForThisSession(bool countAsReplicationTraffic) {
        _countAsReplicationCompressionTraffic = countAsReplicationTraffic;
    }

    bool countsAsReplicationCompressionTrafficForThisSession() const {
        return _countAsReplicationCompressionTraffic;
    }

    /*
     * Called by a client that has received a "hello" response (received after calling
     * clientBegin) and wants to finish negotiating compression.
     *
     * This looks for a BSON array called "compression" with the server's list of
     * requested algorithms. The first algorithm in that array will be used in subsequent calls
     * to compressMessage.
     */
    void clientFinish(const BSONObj& input);

    /*
     * Called by a server that has received a "hello" request.
     *
     * A client-advertised compressor is accepted only if it is BOTH permitted for this connection
     * type AND registered process-wide. The candidate set is chosen by 'serverCompressorAllowList':
     *   - boost::none (default): external, client-facing connection. The candidate set is
     *     net.compression.compressors (MessageCompressorRegistry::getNetCompressorNames()). When
     *     net.compression.compressors: disabled this set is empty, so such connections are always
     *     negotiated uncompressed, independently of replication.networkCompression.compressors.
     *   - engaged: internal replica-set connection. The provided list is the candidate set
     *     (replication.networkCompression.compressors for this node). An empty list forces the
     *     connection uncompressed.
     * This is what lets net.compression.compressors and replication.networkCompression.compressors
     * negotiate independently.
     *
     * If no compressors are configured that match those requested by the client, then it will
     * not append anything to the BSONObjBuilder output.
     */
    void serverNegotiate(const boost::optional<std::vector<std::string_view>>& clientCompressors,
                         BSONObjBuilder* result,
                         const boost::optional<std::vector<std::string>>&
                             serverCompressorAllowList = boost::none);

    /*
     * Returns a new Message containing the compressed contentx of 'msg'. If compressorId is null,
     * then it selects the first negotiated compressor. Otherwise, it uses the compressor with the
     * given identifier. It is intended that this value echo back a value returned as the out
     * parameter value for compressorId from a call to decompressMessage.
     *
     * If _negotiated is empty (meaning compression was not negotiated or is not supported), then
     * it will return a ref-count bumped copy of the input message.
     *
     * If an error occurs in the compressor, it will return a Status error.
     */
    StatusWith<Message> compressMessage(const Message& msg,
                                        const MessageCompressorId* compressorId = nullptr);

    /*
     * Returns a new Message containing the decompressed copy of the input message.
     *
     * If the compressor is not supported or decompression fails, it returns a Status error.
     * Once this connection has established a permit list via negotiation, only compressors permitted
     * for this connection may be decompressed. Legacy/direct callers without a permit list keep the
     * historical registry-wide behavior.
     *
     * If 'compressorId' is non-null, it is populated with the compressor used and may be fed back
     * into compressMessage(), subject to the same per-connection permit-list check.
     */
    StatusWith<Message> decompressMessage(const Message& msg,
                                          MessageCompressorId* compressorId = nullptr,
                                          size_t maxMessageSize = MaxMessageSizeBytes);

    const std::vector<MessageCompressorBase*>& getNegotiatedCompressors() const;

    // True once this connection has an explicit compressor-id permit list, established by
    // serverNegotiate() or the clientBegin()/clientFinish() handshake. Used to reject compressed
    // frames before negotiation completes.
    bool hasCompressorPermitListForThisSession() const {
        return _permittedCompressorIds.has_value();
    }

    static MessageCompressorManager& forSession(const std::shared_ptr<transport::Session>& session);

private:
    // Per-connection compressor-id permit list. Once engaged, only these ids may appear on the
    // wire for this connection. This is needed because the process-wide registry may contain both
    // net and replication compressors, while each connection must enforce its own candidate set.
    // boost::none preserves legacy/direct-call behavior before negotiation.
    boost::optional<std::array<bool, std::numeric_limits<MessageCompressorId>::max() + 1>>
        _permittedCompressorIds;

    bool _isCompressorPermittedForThisSession(MessageCompressorId id) const {
        if (!_permittedCompressorIds) {
            return true;
        }
        return (*_permittedCompressorIds)[id];
    }

    std::vector<std::string> _advertisedCompressors;
    std::vector<MessageCompressorBase*> _negotiated;
    MessageCompressorRegistry* _registry;
    // Note: _offerCompressionOnClientBegin and _allowListedCompressors are declared
    // near the top of the class so that inline member functions can see them without
    // relying on delayed parsing of class-body member initializers.
};

}  // namespace mongo
