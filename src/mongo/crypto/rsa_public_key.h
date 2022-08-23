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

#include "mongo/base/data_range.h"
#include "mongo/base/string_data.h"
#include "mongo/util/base64.h"

namespace mongo::crypto {
/**
 * Provides an interface for managing parameters to an RSA signing operation.
 * Note that this key contains public material only, and is not suitable for decryption.
 */
class RsaPublicKey {
public:
    /**
     * Creates an `RsaPublicKey` instance identified by the opaque string name {keyId}.
     * The RSA operation parameters of {E} and {N} must be passed as Base64URL encoded values (RFC
     * 4648 §5).
     */
    RsaPublicKey(StringData keyId, StringData e, StringData n);
    std::size_t getKeySizeBytes() const {
        return _n.size();
    }

    ConstDataRange getE() const {
        return ConstDataRange(_e);
    }

    ConstDataRange getN() const {
        return ConstDataRange(_n);
    }

    StringData getKeyId() const {
        return _keyId;
    }

private:
    std::vector<std::uint8_t> _e;
    std::vector<std::uint8_t> _n;
    std::string _keyId;
};

}  // namespace mongo::crypto
