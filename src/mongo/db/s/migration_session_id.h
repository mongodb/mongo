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

#include "mongo/base/string_data.h"

#include <string>

#include <boost/optional.hpp>

namespace mongo {

class BSONObj;
class BSONObjBuilder;
template <typename T>
class StatusWith;

/**
 * Encapsulates the logic for generating, parsing and comparing migration sessions. The migration
 * session id is a unique identifier for a particular moveChunk command and is exchanged as part of
 * all communication between the donor and recipient shards.
 */
class MigrationSessionId {
public:
    MigrationSessionId() = default;

    /**
     * Constructs a new migration session identifier with the following format:
     *  DonorId_RecipientId_UniqueIdentifier
     */
    static MigrationSessionId generate(StringData donor, StringData recipient);

    /**
     * Extracts the session id from BSON. If the session id is missing from the BSON contents,
     * returns a NoSuchKey error.
     */
    static StatusWith<MigrationSessionId> extractFromBSON(const BSONObj& obj);

    /**
     * Same as extractFromBSON, but throws on error.
     * Function signature is compatible for idl.
     */
    static MigrationSessionId parseFromBSON(const BSONObj& obj);

    /**
     * Compares two session identifiers. Two idendifiers match only if they are the same.
     */
    bool matches(const MigrationSessionId& other) const;

    /**
     * Appends the migration session id to the specified builder.
     */
    void append(BSONObjBuilder* builder) const;

    std::string toString() const;

    static MigrationSessionId fromString(StringData sessionId) {
        MigrationSessionId id(std::string{sessionId});
        return id;
    }

private:
    explicit MigrationSessionId(std::string sessionId);

    std::string _sessionId;
};

}  // namespace mongo
