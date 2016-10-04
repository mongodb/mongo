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

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;

namespace rpc {

/**
 * This class encapsulates the metadata sent between shard mongods and mongos on every command
 * request and response, containing metadata information about the config servers.
 *
 * format:
 * configsvr: {
 *     opTime: {ts: Timestamp(0, 0), t: 0}
 * }
 */
class ConfigServerMetadata {
public:
    static const OperationContext::Decoration<ConfigServerMetadata> get;

    ConfigServerMetadata() = default;
    explicit ConfigServerMetadata(repl::OpTime opTime);

    /**
     * Parses the metadata from the given metadata object.
     * Returns a non-ok status on parse error.
     * If no metadata is found, returns a default-constructed ConfigServerMetadata.
     */
    static StatusWith<ConfigServerMetadata> readFromMetadata(const BSONObj& metadataObj);

    /**
     * Parses ConfigServerMetadata from a pre-extracted BSONElement. When reading a metadata object,
     * this form is more efficient as it permits parsing the metadata in one pass.
     */
    static StatusWith<ConfigServerMetadata> readFromMetadata(const BSONElement& metadataElem);

    /**
     * Writes the metadata to the given BSONObjBuilder for building a command request or response
     * metadata.
     * Only valid to call if _opTime is initialized.
     */
    void writeToMetadata(BSONObjBuilder* builder) const;

    /**
     * Returns the OpTime of the most recent operation on the config servers that this
     * shard has seen.
     */
    boost::optional<repl::OpTime> getOpTime() const {
        return _opTime;
    }

    static StringData fieldName() {
        return "configsvr";
    }

private:
    boost::optional<repl::OpTime> _opTime;
};

}  // namespace rpc
}  // namespace mongo
