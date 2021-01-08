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

#include <string>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace auth {

/**
 * IDL support class for proxying different ways of passing/returning
 * a SASL payload.
 *
 * Payload may be either a base64 encoded string,
 * or a BinDataGeneral containing the raw payload bytes.
 */
class SaslPayload {
public:
    SaslPayload() = default;

    explicit SaslPayload(std::string data) : _payload(std::move(data)) {}

    bool getSerializeAsBase64() const {
        return _serializeAsBase64;
    }

    void serializeAsBase64(bool opt) {
        _serializeAsBase64 = opt;
    }

    const std::string& get() const {
        return _payload;
    }

    static SaslPayload parseFromBSON(const BSONElement& elem);
    void serializeToBSON(StringData fieldName, BSONObjBuilder* bob) const;
    void serializeToBSON(BSONArrayBuilder* bob) const;

private:
    bool _serializeAsBase64 = false;
    std::string _payload;
};

}  // namespace auth
}  // namespace mongo
