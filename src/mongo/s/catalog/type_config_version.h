/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/namespace_string.h"

namespace mongo {

/**
 * This class represents the layout and contents of documents contained in the
 * config.version collection. All manipulation of documents coming from that
 * collection should be done with this class.
 */
class VersionType {
public:
    // TODO SERVER-68889 remove once 7.0 becomes last LTS
    static constexpr int MIN_COMPATIBLE_CONFIG_VERSION = 5;
    static constexpr int CURRENT_CONFIG_VERSION = 6;

    // Name of the version collection in the config server.
    static const NamespaceString ConfigNS;

    // Field names and types in the version collection type.
    // TODO SERVER-68889 remove once 7.0 becomes last LTS
    static const BSONField<int> minCompatibleVersion;
    static const BSONField<int> currentVersion;

    static const BSONField<OID> clusterId;

    /**
     * Returns the BSON representation of the entry.
     */
    BSONObj toBSON() const;

    /**
     * Clears the internal state.
     */
    void clear();

    /**
     * Copies all the fields present in 'this' to 'other'.
     */
    void cloneTo(VersionType* other) const;

    /**
     * Constructs a new ChangeLogType object from BSON.
     * Also does validation of the contents.
     */
    static StatusWith<VersionType> fromBSON(const BSONObj& source);

    /**
     * Returns OK if all fields have been set. Otherwise, returns NoSuchKey
     * and information about the first field that is missing.
     */
    Status validate() const;

    /**
     * Returns a std::string representation of the current internal state.
     */
    std::string toString() const;

    const boost::optional<int>& getMinCompatibleVersion() const {
        return _minCompatibleVersion;
    }
    void setMinCompatibleVersion(boost::optional<int> minCompatibleVersion);

    const boost::optional<int>& getCurrentVersion() const {
        return _currentVersion;
    }

    void setCurrentVersion(boost::optional<int> currentVersion);

    const OID& getClusterId() const {
        return _clusterId;
    }
    void setClusterId(const OID& clusterId);

private:
    // Convention: (M)andatory, (O)ptional, (S)pecial rule.

    // TODO SERVER-68889 remove once 7.0 becomes last LTS
    boost::optional<int> _minCompatibleVersion;
    // TODO SERVER-68889 remove once 7.0 becomes last LTS
    boost::optional<int> _currentVersion;

    // (M) clusterId
    OID _clusterId;
};

}  // namespace mongo
