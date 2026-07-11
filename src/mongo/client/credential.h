// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/auth_mechanism.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] auth {

/** Typed representation of client authentication credentials. */
struct Credential {
    AuthMechanism mechanism;

    /** Auth-source database. Uses mechanism default ($external or admin) when absent. */
    boost::optional<std::string> db;

    /** Required for SCRAM, PLAIN, GSSAPI; absent for X.509 / AWS / OIDC. */
    boost::optional<std::string> username;

    /** Required for SCRAM and PLAIN; absent for others. */
    boost::optional<std::string> password;

    /** Mechanism-specific options. Not required, but not marked as optional for ergonomics. */
    BSONObj mechanismProperties;

    /**
     * Parse and validate BSON into a Credential. Returns an error if a required field is missing or
     * if both "db" and the legacy "userSource" field are present.
     */
    static StatusWith<Credential> fromBSON(const BSONObj& params);
};

}  // namespace auth
}  // namespace mongo
