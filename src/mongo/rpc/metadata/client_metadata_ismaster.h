/**
* Copyright (C) 2016 MongoDB Inc.
*
* This program is free software: you can redistribute it and/or  modify
* it under the terms of the GNU Affero General Public License, version 3,
* as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Affero General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
* As a special exception, the copyright holders give permission to link the
* code of portions of this program with the OpenSSL library under certain
* conditions as described in each individual source file and distribute
* linked combinations including the program with the OpenSSL library. You
* must comply with the GNU Affero General Public License in all respects
* for all of the code used other than as permitted herein. If you modify
* file(s) with this exception, you may extend this exception to your
* version of the file(s), but you are not obligated to do so. If you do not
* wish to do so, delete this exception statement from your version. If you
* delete this exception statement from all source files in the program,
* then also delete it in the license file.
*/

#pragma once

#include <boost/optional.hpp>
#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/rpc/metadata/client_metadata.h"

namespace mongo {

class Client;

/**
 * ClientMetadataIsMasterState is responsible for tracking whether the client metadata document has
 * been received by the specified Client object.
 */
class ClientMetadataIsMasterState {
    MONGO_DISALLOW_COPYING(ClientMetadataIsMasterState);

public:
    ClientMetadataIsMasterState() = default;

    static ClientMetadataIsMasterState& get(Client* client);

    /**
     * Get the optional client metadata object.
     */
    const boost::optional<ClientMetadata>& getClientMetadata() const;

    /**
     * Set the optional client metadata object.
     */
    static void setClientMetadata(Client* client, boost::optional<ClientMetadata> clientMetadata);

    /**
     * Check a flag to indicate that isMaster has been seen for this Client.
     */
    bool hasSeenIsMaster() const;

    /**
     * Set a flag to indicate that isMaster has been seen for this Client.
     */
    void setSeenIsMaster();

    /**
     * Read from the $client section of OP_Command's metadata.
     *
     * Returns an error if the $client section is not valid. It is valid for it to not exist though.
     *
     * Thread-Safety:
     *   None - must be only be read and written from the thread owning "Client".
     */
    static Status readFromMetadata(OperationContext* opCtx, BSONElement& elem);

    /**
     * Write the $client section to OP_Command's metadata if there is a non-empty client metadata
     * connection with the current client.
     *
     * Thread-Safety:
     *   None - must be only be read and written from the thread owning "Client".
     */
    static void writeToMetadata(OperationContext* opCtx, BSONObjBuilder* builder);

private:
    // Optional client metadata document.
    // Set if client sees isMaster cmd or as part of OP_Command processing.
    // Thread-Safety:
    //   Can be read and written from the thread owning "Client".
    //   Can be read from other threads if they hold the "Client" lock.
    boost::optional<ClientMetadata> _clientMetadata{boost::none};

    // Indicates whether we have seen an is master for this client.
    // Thread-Safety:
    //   None - must be only be read and written from the thread owning "Client".
    bool _hasSeenIsMaster{false};
};

}  // namespace mongo
