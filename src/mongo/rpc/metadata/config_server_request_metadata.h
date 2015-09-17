/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <boost/optional.hpp>

#include "mongo/db/repl/optime.h"

namespace mongo {

class BSONObj;
template <typename T>
class StatusWith;

namespace rpc {

/**
 * This class encapsulates the extra information that mongos may attach to commands it sends to
 * mongods, containing metadata information about the config servers.
 *
 * format:
 * configsvrOpTime: {ts: Timestamp(0, 0), t: 0}
 *
 * TODO(SERVER-20442): Currently this extracts the config server information from the main command
 * description rather than the actual OP_COMMAND metadata section.  Ideally this information
 * should be in the metadata, but we currently have no good way to add metadata to all commands
 * being *sent* to another server.
 */
class ConfigServerRequestMetadata {
public:
    ConfigServerRequestMetadata() = default;
    explicit ConfigServerRequestMetadata(repl::OpTime opTime);

    /**
     * Parses the request metadata from the given command object.
     * Returns a non-ok status on parse error.
     * If no metadata is found, returns a default-constructed ConfigServerRequestMetadata.
     */
    static StatusWith<ConfigServerRequestMetadata> readFromCommand(const BSONObj& doc);

    /**
     * Writes the request metadata to the given BSONObjBuilder for building a command request.
     * Only valid to call if _opTime is initialized.
     */
    void writeToCommand(BSONObjBuilder* builder) const;

    /**
     * Returns the OpTime of the most recent operation on the config servers that this
     * shard has seen.
     */
    boost::optional<repl::OpTime> getOpTime() const {
        return _opTime;
    }

private:
    const boost::optional<repl::OpTime> _opTime = boost::none;
};

}  // namespace rpc
}  // namespace mongo
