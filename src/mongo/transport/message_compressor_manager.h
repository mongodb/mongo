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

#include <memory>
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
     * Called by a client constructing a "hello" request. This function will append the result
     * of _registry->getCompressorNames() to the BSONObjBuilder as a BSON array. If no compressors
     * are configured, it won't append anything.
     */
    void clientBegin(BSONObjBuilder* output);

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
     * If no compressors are configured that match those requested by the client, then it will
     * not append anything to the BSONObjBuilder output.
     */
    void serverNegotiate(const boost::optional<std::vector<std::string_view>>& clientCompressors,
                         BSONObjBuilder*);

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
     * This can be called before Compression is negotiated with the client. This class has a
     * pointer to the global MessageCompressorRegistry and can lookup any message's compressor
     * by ID number through that registry. As long as the compressor has been enabled process-wide,
     * it can decompress any message without negotiation.
     *
     * If the 'compressorId' parameter is non-null, it will be populated with the compressor
     * used. If 'decompressMessage' returns succesfully, then that value can be fed back into
     * compressMessage, ensuring that the same compressor is used on both sides of a conversation.
     */
    StatusWith<Message> decompressMessage(const Message& msg,
                                          MessageCompressorId* compressorId = nullptr,
                                          size_t maxMessageSize = MaxMessageSizeBytes);

    const std::vector<MessageCompressorBase*>& getNegotiatedCompressors() const;

    static MessageCompressorManager& forSession(const std::shared_ptr<transport::Session>& session);

private:
    std::vector<MessageCompressorBase*> _negotiated;
    MessageCompressorRegistry* _registry;
};

}  // namespace mongo
