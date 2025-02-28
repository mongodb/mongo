/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/version_context.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {

using FCVSnapshot = ServerGlobalParams::FCVSnapshot;

VersionContext::VersionContext(FCV fcv)
    : _metadataOrTag(std::in_place_type<VersionContextMetadata>, fcv) {}

VersionContext::VersionContext(FCVSnapshot fcv)
    : _metadataOrTag(std::in_place_type<VersionContextMetadata>, fcv.getVersion()) {}

VersionContext::VersionContext(const BSONObj& bsonObject) {
    if (bsonObject.isEmpty()) {
        return;
    }
    _metadataOrTag =
        VersionContextMetadata::parse(IDLParserContext("VersionContextMetadata"), bsonObject);
}

VersionContext& VersionContext::operator=(const VersionContext& other) {
    _assertOFCVNotInitialized();
    _metadataOrTag = other._metadataOrTag;
    return *this;
}

void VersionContext::setOperationFCV(FCV fcv) {
    _assertOFCVNotInitialized();
    _metadataOrTag.emplace<VersionContextMetadata>(fcv);
}

void VersionContext::setOperationFCV(FCVSnapshot fcv) {
    _assertOFCVNotInitialized();
    _metadataOrTag.emplace<VersionContextMetadata>(fcv.getVersion());
}

boost::optional<FCVSnapshot> VersionContext::getOperationFCV() const {
    return visit(OverloadedVisitor{[](const VersionContextMetadata& metadata) {
                                       return boost::optional<FCVSnapshot>{metadata.getOFCV()};
                                   },
                                   [](auto&&) {
                                       return boost::optional<FCVSnapshot>{};
                                   }},
                 _metadataOrTag);
}

BSONObj VersionContext::toBSON() const {
    return visit(
        OverloadedVisitor{[](OperationWithoutOFCVTag) { return BSONObj(); },
                          [](const VersionContextMetadata& metadata) { return metadata.toBSON(); },
                          [](auto&&) -> BSONObj {
                              MONGO_UNREACHABLE;
                          }},
        _metadataOrTag);
}

void VersionContext::_assertOFCVNotInitialized() const {
    uassert(ErrorCodes::AlreadyInitialized,
            "The operation FCV has already been set.",
            std::holds_alternative<OperationWithoutOFCVTag>(_metadataOrTag));
}

}  // namespace mongo
