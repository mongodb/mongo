// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>
#include <utility>

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
    void serializeToBSON(std::string_view fieldName, BSONObjBuilder* bob) const;
    void serializeToBSON(BSONArrayBuilder* bob) const;

private:
    bool _serializeAsBase64 = false;
    std::string _payload;
};

}  // namespace auth
}  // namespace mongo
