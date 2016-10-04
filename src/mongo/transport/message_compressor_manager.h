/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/transport/message_compressor_base.h"

#include <vector>

namespace mongo {

class BSONObj;
class BSONObjBuilder;
class Message;
class MessageCompressorRegistry;

class MessageCompressorManager {
    MONGO_DISALLOW_COPYING(MessageCompressorManager);

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
     * Called by a client constructing an isMaster request. This function will append the result
     * of _registry->getCompressorNames() to the BSONObjBuilder as a BSON array. If no compressors
     * are configured, it won't append anything.
     */
    void clientBegin(BSONObjBuilder* output);

    /*
     * Called by a client that has received an isMaster response (received after calling
     * clientBegin) and wants to finish negotiating compression.
     *
     * This looks for a BSON array called "compression" with the server's list of
     * requested algorithms. The first algorithm in that array will be used in subsequent calls
     * to compressMessage.
     */
    void clientFinish(const BSONObj& input);

    /*
     * Called by a server that has received an isMaster request.
     *
     * This looks for a BSON array called "compression" in input and appends the union of that
     * array and the result of _registry->getCompressorNames(). The first name in the compression
     * array in input will be used in subsequent calls to compressMessage
     *
     * If no compressors are configured that match those requested by the client, then it will
     * not append anything to the BSONObjBuilder output.
     */
    void serverNegotiate(const BSONObj& input, BSONObjBuilder* output);

    /*
     * Returns a new Message compressed with the compressor pointed to by _preferred.
     *
     * If _negotiated is empty (meaning compression was not negotiated or is not supported), then
     * it will return a ref-count bumped copy of the input message.
     *
     * If an error occurs in the compressor, it will return a Status error.
     */
    StatusWith<Message> compressMessage(const Message& msg);

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
     */
    StatusWith<Message> decompressMessage(const Message& msg);

private:
    std::vector<MessageCompressorBase*> _negotiated;
    MessageCompressorRegistry* _registry;
};

}  // namespace mongo
