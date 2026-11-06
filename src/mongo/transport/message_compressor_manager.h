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
     * Called by a client constructing a "hello" request. Advertises the net compressors, or this
     * session's allow-list if one is set. Nothing is advertised after
     * disableCompressionForThisSession().
     */
    void clientBegin(BSONObjBuilder* output);

    /*
     * Stops clientBegin() from advertising compressors. Takes effect on the next handshake and is
     * kept across reconnects.
     */
    void disableCompressionForThisSession() {
        _offerCompressionOnClientBegin = false;
    }

    /*
     * Undoes disableCompressionForThisSession() for the next handshake.
     */
    void enableCompressionForThisSession() {
        _offerCompressionOnClientBegin = true;
    }

    bool isCompressionOfferedForThisSession() const {
        return _offerCompressionOnClientBegin;
    }

    /*
     * Limits what clientBegin() advertises to these compressors, in this order. Names missing from
     * the registry are skipped. An empty list goes back to the net compressors.
     */
    void setCompressorAllowListForThisSession(std::vector<std::string> allowList) {
        _allowListedCompressors = std::move(allowList);
    }

    /*
     * Hello field sent by replication data-plane clients. Older servers ignore it.
     */
    static constexpr auto kReplicationCompressionClientFieldName = "replicationCompressionClient";

    /*
     * Makes clientBegin() send kReplicationCompressionClientFieldName.
     */
    void markReplicationClientForThisSession(bool isReplicationClient) {
        _isReplicationClient = isReplicationClient;
    }

    /*
     * Counts this connection's compression bytes in serverStatus().repl.compression as well. The
     * server side of a replication connection sets this without sending the hello field.
     */
    void countAsReplicationCompressionTrafficForThisSession(bool countAsReplicationTraffic) {
        _countAsReplicationCompressionTraffic = countAsReplicationTraffic;
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
     * Client compressors are matched against 'serverCompressorAllowList', or against the net
     * compressors when it is boost::none. An empty allow-list negotiates no compression. The
     * allow-list used for a connection is kept for later hellos on it.
     *
     * If no compressors are configured that match those requested by the client, then it will
     * not append anything to the BSONObjBuilder output.
     */
    void serverNegotiate(
        const boost::optional<std::vector<std::string_view>>& clientCompressors,
        BSONObjBuilder* result,
        const boost::optional<std::vector<std::string>>& serverCompressorAllowList = boost::none);

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
     * After negotiation, only compressors allowed for this connection are accepted. Before that,
     * any registered compressor is accepted.
     *
     * If 'compressorId' is non-null, it is populated with the compressor used and may be fed back
     * into compressMessage().
     */
    StatusWith<Message> decompressMessage(const Message& msg,
                                          MessageCompressorId* compressorId = nullptr,
                                          size_t maxMessageSize = MaxMessageSizeBytes);

    const std::vector<MessageCompressorBase*>& getNegotiatedCompressors() const;

    // True once negotiation has set the compressors allowed on this connection.
    bool hasCompressorPermitListForThisSession() const {
        return _permittedCompressorIds.has_value();
    }

    static MessageCompressorManager& forSession(const std::shared_ptr<transport::Session>& session);

private:
    // Allows exactly the registered compressors named in 'names' on this connection.
    void _setPermittedCompressors(const std::vector<std::string>& names);

    // The registry holds both net and replication compressors, so each connection keeps the ids it
    // may use. boost::none means negotiation has not happened yet.
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

    bool _offerCompressionOnClientBegin{true};
    std::vector<std::string> _allowListedCompressors;
    bool _isReplicationClient{false};
    bool _countAsReplicationCompressionTraffic{false};

    // Server side only: the allow-list this connection negotiated against. Later hellos often omit
    // the replication field, so this is kept instead of falling back to the net compressors.
    boost::optional<std::vector<std::string>> _serverReplicationCompressorAllowListForThisSession;
};

}  // namespace mongo
