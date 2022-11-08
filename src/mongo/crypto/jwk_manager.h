/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <map>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"

namespace mongo::crypto {

class JWKManager {
public:
    JWKManager() = default;
    /**
    Fetch a JWKS file from the specified URL, parse them as keys,
    and instantiate JWSValidator instances.
    */
    explicit JWKManager(StringData source);

    /**
    Given a unique keyId it will return the matching JWK.
    If no key is found for the given keyId, a uassert will be thrown.
    */
    const BSONObj& getKey(StringData keyId) const;

private:
    // Map<keyId, JWKRSA>
    std::map<std::string, BSONObj> _keyMaterial;  // From last load
    std::string _keyURI;
};

}  // namespace mongo::crypto
