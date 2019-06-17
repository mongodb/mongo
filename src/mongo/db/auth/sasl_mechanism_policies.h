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
#include "mongo/crypto/hash_block.h"
#include "mongo/db/auth/sasl_mechanism_registry.h"

namespace mongo {

struct PLAINPolicy {
    static constexpr StringData getName() {
        return "PLAIN"_sd;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{};
    }
    static int securityLevel() {
        return 0;
    }
    static constexpr bool isInternalAuthMech() {
        return false;
    }
};

struct SCRAMSHA1Policy {
    using HashBlock = SHA1Block;

    static constexpr StringData getName() {
        return "SCRAM-SHA-1"_sd;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth};
    }
    static int securityLevel() {
        return 1;
    }
    static constexpr bool isInternalAuthMech() {
        return true;
    }
};

struct SCRAMSHA256Policy {
    using HashBlock = SHA256Block;

    static constexpr StringData getName() {
        return "SCRAM-SHA-256"_sd;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth};
    }
    static int securityLevel() {
        return true;
    }
    static constexpr bool isInternalAuthMech() {
        return true;
    }
};

struct GSSAPIPolicy {
    static constexpr StringData getName() {
        return "GSSAPI"_sd;
    }
    static SecurityPropertySet getProperties() {
        return SecurityPropertySet{SecurityProperty::kNoPlainText, SecurityProperty::kMutualAuth};
    }
    static int securityLevel() {
        return 3;
    }
    static constexpr bool isInternalAuthMech() {
        return false;
    }
};


}  // namespace mongo
