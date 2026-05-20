/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/auth_mechanism.h"
#include "mongo/util/modules.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace MONGO_MOD_PUBLIC auth {

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

}  // namespace MONGO_MOD_PUBLIC auth
}  // namespace mongo
