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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
template <typename T>
class StatusWith;

/**
 * Encapsulates the logic for generating, parsing and comparing migration sessions. The migration
 * session id is a unique identifier for a particular moveChunk command and is exchanged as part of
 * all communication between the source and donor shards.
 */
class MigrationSessionId {
public:
    /**
     * Constructs a new migration session identifier with the following format:
     *  DonorId_RecipientId_UniqueIdentifier
     */
    static MigrationSessionId generate(StringData donor, StringData recipient);

    /**
     * Extracts the session id from BSON. If the session id is missing from the BSON contents,
     * returns an empty MigrationSessionId.
     */
    static StatusWith<MigrationSessionId> extractFromBSON(const BSONObj& obj);

    /**
     * Compares two session identifiers. Two idendifiers match if either both are empty (_sessionId
     * is not set) or if the session ids match.
     */
    bool matches(const MigrationSessionId& other) const;

    /**
     * Appends the migration session id to the specified builder.
     */
    void append(BSONObjBuilder* builder) const;

    std::string toString() const;

    bool isEmpty() const;

private:
    MigrationSessionId();
    explicit MigrationSessionId(std::string sessionId);

    boost::optional<std::string> _sessionId{boost::none};
};

}  // namespace mongo
