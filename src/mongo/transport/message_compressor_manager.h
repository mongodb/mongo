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
    // SERVER-130410: true only on the manager of a replication data-plane client connection (the
    // oplog fetcher, initial-sync cloner, or rollback remote oplog reader, marked via
    // markReplicationClientForThisSession()). When set, clientBegin() adds the
    // "replicationCompressionClient" marker to the hello request so the server routes this
    // connection (and ONLY this connection) through the replication candidate
    // set. Other internal connections (heartbeats, shard RPC) leave this false and are negotiated
    // like any external client (net.compression.compressors), restoring pre-SERVER-130410 behavior.
    bool _isReplicationClient{false};

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
     * Called by a client constructing a "hello" request. With no per-session state this appends
     * the client-facing net set (MessageCompressorRegistry::getNetCompressorNames(), i.e.
     * net.compression.compressors) to the BSONObjBuilder as a BSON array. If no net compressors
     * are configured, it won't append anything.
     *
     * If a per-session allow-list has been set (setCompressorAllowListForThisSession(), used by
     * replication data-plane connections), it instead advertises the intersection of that list with
     * the process-wide capability set (getCompressorNames(), the net union replication union). This
     * is what lets a replication-only compressor be advertised even when net.compression.compressors:
     * disabled (SERVER-130410).
     *
     * If disableCompressionForThisSession() has been called on this manager, this function will
     * not advertise any compressor to the server, and the resulting connection will be negotiated
     * uncompressed regardless of any setting.
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
     * Restricts the compressors advertised by clientBegin() on this manager to the given
     * allow-list, intersected with the process-wide capability set (getCompressorNames(), the
     * union of net.compression.compressors and replication.networkCompression.compressors). Names
     * not present in that union are silently ignored so that a per-session preference can never
     * widen the process capability surface. Intersecting against the union (rather than only the
     * net set) is what lets a replication-only compressor be advertised even when
     * net.compression.compressors: disabled. Passing an empty vector clears the allow-list and
     * returns to advertising the net set.
     *
     * This is meant for replication data-plane connections using
     * replication.networkCompression.compressors YAML/setParameter (SERVER-130410) which lets
     * an operator negotiate sync-source data transfer with a subset of the process-wide compressor
     * list (e.g. force zstd on replication while the client-facing port keeps snappy,zstd,zlib).
     * It has no effect on any other connection.
     */
    void setCompressorAllowListForThisSession(std::vector<std::string> allowList) {
        _allowListedCompressors = std::move(allowList);
    }

    const std::vector<std::string>& getCompressorAllowListForThisSession() const {
        return _allowListedCompressors;
    }

    /*
     * Name of the boolean "hello" field used by replication data-plane clients (oplog fetcher,
     * initial-sync cloner, rollback remote oplog reader). internalClient is too broad: heartbeats,
     * mongos-to-mongod traffic, and shard RPC are internal too but must keep using
     * net.compression.compressors. This marker routes only replication data-plane connections
     * through the replication compression policy, which may inherit net.compression.compressors.
     * Older servers ignore the unknown field because "hello" is non-strict.
     */
    static constexpr auto kReplicationCompressionClientFieldName = "replicationCompressionClient";

    /*
     * SERVER-130410: Marks (or unmarks) this manager as belonging to a replication data-plane
     * client connection. Set by applyReplicationNetworkCompressionToManager() for the oplog fetcher,
     * the initial-sync cloner, and the rollback remote oplog reader. When marked, clientBegin()
     * appends kReplicationCompressionClientFieldName:true to the hello request. This is orthogonal
     * to the allow-list / suppression state: an "inherit" replication connection is still a
     * replication client even though it advertises the net set. Persists across DBClientConnection
     * auto-reconnects because the manager instance is reused.
     */
    void markReplicationClientForThisSession(bool isReplicationClient) {
        _isReplicationClient = isReplicationClient;
    }

    bool isReplicationClientForThisSession() const {
        return _isReplicationClient;
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
     * negotiate independently. See SERVER-130410.
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
     * If the compressor specified in the input message is not supported, it will return a Status
     * error.
     *
     * If an error occurs in the compressor, it will return a Status error.
     *
     * This class has a pointer to the global MessageCompressorRegistry and can look up a message's
     * compressor by ID number through that registry. Callers that have not engaged a per-connection
     * permit list keep the historical negotiation-agnostic behavior for any process-wide registered
     * compressor. Once serverNegotiate() or the clientBegin()/clientFinish() handshake has
     * established this connection's permit list, only compressors permitted for this connection may
     * be decompressed.
     *
     * If the 'compressorId' parameter is non-null, it will be populated with the compressor
     * used. If 'decompressMessage' returns successfully, then that value can be fed back into
     * compressMessage, ensuring that the same compressor is used on both sides of a conversation
     * subject to the same per-connection permit-list check.
     */
    StatusWith<Message> decompressMessage(const Message& msg,
                                          MessageCompressorId* compressorId = nullptr,
                                          size_t maxMessageSize = MaxMessageSizeBytes);

    const std::vector<MessageCompressorBase*>& getNegotiatedCompressors() const;

    // SERVER-130410: true once this connection has established a compressor-id permit list. On the
    // server side this happens via serverNegotiate(); on the client side it is initialized empty by
    // clientBegin() and finalized by clientFinish(). Used by the ingress workflow to reject
    // OP_COMPRESSED messages that arrive before any compression negotiation has run; otherwise a
    // pre-negotiation frame would fall back to the process-wide union registry and could use a
    // replication-only compressor on an external connection.
    bool hasCompressorPermitListForThisSession() const {
        return _permittedCompressorIds.has_value();
    }

    static MessageCompressorManager& forSession(const std::shared_ptr<transport::Session>& session);

private:
    // SERVER-130410: per-connection permit list of compressor ids that are allowed to appear on the
    // wire for THIS connection. On the server it is engaged by serverNegotiate() from the
    // connection's candidate set 'allowed' (net.compression.compressors for external clients,
    // replication.networkCompression.compressors for replication connections) intersected with the
    // process-wide registry. On the client it is initialized empty by clientBegin() and populated by
    // clientFinish() from the compressors the client both advertised and accepted from the server.
    //
    // Rationale: the process-wide registry is the union net union replication, so it is larger than
    // any single connection's candidate set. Before SERVER-130410 registry == the single candidate
    // set, which made the negotiation-agnostic decompressMessage()/echo-back compressMessage()
    // lookups (which consult the registry directly) safe. The union broke that invariant. This
    // permit list re-establishes "registry-visible == this connection's candidate set" per connection.
    // When boost::none (legacy/direct callers that have not negotiated) the check below returns true
    // and the original full-registry semantics are preserved unchanged.
    boost::optional<std::array<bool, std::numeric_limits<MessageCompressorId>::max() + 1>>
        _permittedCompressorIds;

    // Returns true if 'id' may be used on the wire for this connection. Returns true unconditionally
    // when the permit list has not been engaged, preserving pre-SERVER-130410 behavior only for
    // legacy/direct callers that have not run compression negotiation.
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
