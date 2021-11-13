/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
namespace mongo::sdam {

// Comparable pair or ElectionId (term) and SetVersion.
struct ElectionIdSetVersionPair {
    boost::optional<mongo::OID> electionId;
    boost::optional<int> setVersion;

    bool allDefined() const {
        return electionId && setVersion;
    }

    bool anyDefined() const {
        return electionId || setVersion;
    }

    bool anyUndefined() const {
        return !electionId || !setVersion;
    }

    BSONObj toBSON() const;
};

inline bool operator<(const ElectionIdSetVersionPair& p1, const ElectionIdSetVersionPair& p2) {
    if (p1.anyUndefined() && p2.allDefined()) {
        return true;
    }

    return (p1.electionId < p2.electionId) ||
        ((p1.electionId == p2.electionId) && (p1.setVersion < p2.setVersion));
}

inline bool operator>(const ElectionIdSetVersionPair& p1, const ElectionIdSetVersionPair& p2) {
    return p2 < p1;
}

// Equality operator is not equivalent to "!< && !>" because of grey area of undefined values.
inline bool operator==(const ElectionIdSetVersionPair& p1, const ElectionIdSetVersionPair& p2) {
    return ((p1.electionId == p2.electionId) && (p1.setVersion == p2.setVersion));
}

inline bool setVersionWentBackwards(const ElectionIdSetVersionPair& current,
                                    const ElectionIdSetVersionPair& incoming) {
    return current.setVersion && incoming.setVersion && *current.setVersion > *incoming.setVersion;
}

inline BSONObj ElectionIdSetVersionPair::toBSON() const {
    BSONObjBuilder bob;
    if (electionId) {
        bob.append("electionId", *electionId);
    }
    if (setVersion) {
        bob.append("setVersion", *setVersion);
    }
    return bob.obj();
}

}  // namespace mongo::sdam
