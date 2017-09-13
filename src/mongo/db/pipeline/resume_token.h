/**
 * Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/uuid.h"

namespace mongo {
/**
 * A token passed in by the user to indicate where in the oplog we should start for
 * $changeStream.
 */
class ResumeToken {
public:
    /**
     * The default no-argument constructor is required by the IDL for types used as non-optional
     * fields.
     */
    ResumeToken() = default;
    explicit ResumeToken(const Value& resumeValue);
    bool operator==(const ResumeToken&);

    Timestamp getTimestamp() const {
        return _timestamp;
    }

    UUID getUuid() const {
        return _uuid;
    }

    Document toDocument() const;

    BSONObj toBSON() const;

    /**
     * Parse a resume token from a BSON object; used as an interface to the IDL parser.
     */
    static ResumeToken parse(const BSONObj& obj);

private:
    /**
     * Construct from a BSON object.
     * External callers should use the static ResumeToken::parse(const BSONObj&) method instead.
     */
    explicit ResumeToken(const BSONObj& resumeBson);

    Timestamp _timestamp;
    UUID _uuid;
    Value _documentId;
};
}  // namespace mongo
