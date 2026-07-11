// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <string>
#include <string_view>

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
    static MigrationSessionId generate(std::string_view donor, std::string_view recipient);

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

    static MigrationSessionId fromString(std::string_view sessionId) {
        MigrationSessionId id(std::string{sessionId});
        return id;
    }

private:
    explicit MigrationSessionId(std::string sessionId);

    std::string _sessionId;
};

}  // namespace mongo
