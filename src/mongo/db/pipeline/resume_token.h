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

#include <boost/optional.hpp>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/uuid.h"

namespace mongo {

struct ResumeTokenData {
    ResumeTokenData(){};
    ResumeTokenData(Timestamp clusterTimeIn,
                    Value documentKeyIn,
                    const boost::optional<UUID>& uuidIn)
        : clusterTime(clusterTimeIn), documentKey(std::move(documentKeyIn)), uuid(uuidIn){};

    bool operator==(const ResumeTokenData& other) const;
    bool operator!=(const ResumeTokenData& other) const {
        return !(*this == other);
    };

    Timestamp clusterTime;
    Value documentKey;
    boost::optional<UUID> uuid;
};

std::ostream& operator<<(std::ostream& out, const ResumeTokenData& tokenData);

/**
 * A token passed in by the user to indicate where in the oplog we should start for
 * $changeStream.  This token has the following format:
 * {
 *   _data: <binary data>,
 *   _typeBits: <binary data>
 * }
 * The _data field data is encoded such that byte by byte comparisons provide the correct
 * ordering of tokens.  The _typeBits field may be missing and should not affect token
 * comparison.
 */

class ResumeToken {
public:
    /**
     * The default no-argument constructor is required by the IDL for types used as non-optional
     * fields.
     */
    ResumeToken() = default;

    explicit ResumeToken(const ResumeTokenData& resumeValue);

    bool operator==(const ResumeToken&) const;
    bool operator!=(const ResumeToken&) const;
    bool operator<(const ResumeToken&) const;
    bool operator<=(const ResumeToken&) const;
    bool operator>(const ResumeToken&) const;
    bool operator>=(const ResumeToken&) const;

    /** Three way comparison, returns 0 if *this is equal to other, < 0 if *this is less than
     * other, and > 0 if *this is greater than other.
     */
    int compare(const ResumeToken& other) const;

    Document toDocument() const;

    BSONObj toBSON() const {
        return toDocument().toBson();
    }

    ResumeTokenData getData() const;

    /**
     * Parse a resume token from a BSON object; used as an interface to the IDL parser.
     */
    static ResumeToken parse(const BSONObj& resumeBson) {
        return ResumeToken::parse(Document(resumeBson));
    }

    static ResumeToken parse(const Document& document);

    friend std::ostream& operator<<(std::ostream& out, const ResumeToken& token) {
        return out << token.getData();
    }

    constexpr static StringData kDataFieldName = "_data"_sd;
    constexpr static StringData kTypeBitsFieldName = "_typeBits"_sd;

private:
    explicit ResumeToken(const Document& resumeData);

    Value _keyStringData;
    Value _typeBits;
};
}  // namespace mongo
