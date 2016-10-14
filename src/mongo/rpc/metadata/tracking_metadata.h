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

#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;

namespace rpc {

/**
 * This class encapsulates the metadata sent on every command request and response,
 * containing data to track command execution. The current implementation only applies to commands
 * that go through ShardRemote, which is most sharding catalog operations. It excludes the entire
 * replication subsystem, query-type client commands, and write-type client commands.
 *
 * format:
 * tracking_info: {
 *     operId: ObjectId("") // unique ID for the current operation
 *     operName: string // command name of the current operation
 *     parentOperId: string  // '|' separated chain of the ancestor commands, oldest first
 * }
 */
class TrackingMetadata {
public:
    static const OperationContext::Decoration<TrackingMetadata> get;

    TrackingMetadata() = default;
    explicit TrackingMetadata(OID operId, std::string operName);
    explicit TrackingMetadata(OID operId, std::string operName, std::string parentOperId);

    /**
     * Parses the metadata from the given metadata object.
     * Returns a NoSuchKey error status if it does not have operId or operName set.
     * Returns a TypeMismatch error if operId is not OID and operName or parentOperId are not String
     * If no metadata is found, returns a default-constructed TrackingMetadata.
     */
    static StatusWith<TrackingMetadata> readFromMetadata(const BSONObj& metadataObj);

    /**
     * Parses TrackingMetadata from a pre-extracted BSONElement. When reading a metadata object,
     * this form is more efficient as it permits parsing the metadata in one pass.
     */
    static StatusWith<TrackingMetadata> readFromMetadata(const BSONElement& metadataElem);

    /**
     * Writes the metadata to the given BSONObjBuilder for building a command request or response
     * metadata. Only valid to call if operId and operName are set.
     */
    void writeToMetadata(BSONObjBuilder* builder) const;

    /**
     * Returns the Id of this operation.
     */
    boost::optional<OID> getOperId() const {
        return _operId;
    }

    /**
     * Returns the name of this operation.
     */
    boost::optional<std::string> getOperName() const {
        return _operName;
    }

    /**
     * Returns the parent operId of this operation.
     */
    boost::optional<std::string> getParentOperId() const {
        return _parentOperId;
    }

    static StringData fieldName() {
        return "tracking_info";
    }

    /**
     * Sets operName to name argument. Intended to initialize the metadata when command name is
     * known.
     */
    void initWithOperName(const std::string& name);

    /*
     *  get|set isLogged are used to avoid logging parent metadata more than once.
     */
    bool getIsLogged() const {
        return _isLogged;
    }

    void setIsLogged(bool isLogged) {
        _isLogged = isLogged;
    }

    /*
     *  Builds metadata for child command by updating parentOperId with current operId and
     *  setting operId to a new value.
     */
    TrackingMetadata constructChildMetadata() const;

    std::string toString() const;

    static BSONObj removeTrackingData(BSONObj metadata);

private:
    boost::optional<OID> _operId;
    boost::optional<std::string> _operName;
    boost::optional<std::string> _parentOperId;
    bool _isLogged{false};
};

}  // namespace rpc
}  // namespace mongo
