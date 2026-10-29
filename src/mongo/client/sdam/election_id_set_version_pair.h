// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
