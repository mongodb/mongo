// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/version_context.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/overloaded_visitor.h"

namespace mongo {
namespace {

static auto getVersionContext = OperationContext::declareDecoration<VersionContext>();

}  // namespace

const VersionContext& VersionContext::getDecoration(const OperationContext* opCtx) {
    return getVersionContext(opCtx);
}

VersionContext& VersionContext::_getDecoration(OperationContext* opCtx) {
    return getVersionContext(opCtx);
}

VersionContext::VersionContext(FCV fcv)
    : _metadataOrTag(std::in_place_type<VersionContextMetadata>, fcv) {}

VersionContext::VersionContext(FCVSnapshot fcv)
    : _metadataOrTag(std::in_place_type<VersionContextMetadata>, fcv.getVersion()) {}

VersionContext::VersionContext(const BSONObj& bsonObject) {
    _metadataOrTag = VersionContextMetadata::parse(bsonObject);
}

VersionContext& VersionContext::operator=(const VersionContext& other) {
    if (*this != other) {
        _assertHasNoOperationFCV();
        _metadataOrTag = other._metadataOrTag;
    }
    _canPropagateAcrossShards = other._canPropagateAcrossShards;
    return *this;
}

void VersionContext::setOperationFCV(FCV fcv) {
    if (_isMatchingOFCV(fcv)) {
        return;
    }
    _assertHasNoOperationFCV();
    _metadataOrTag.emplace<VersionContextMetadata>(fcv);
}

void VersionContext::setOperationFCV(FCVSnapshot fcv) {
    setOperationFCV(fcv.getVersion());
}

void VersionContext::resetToOperationWithoutOFCV() {
    invariant(hasOperationFCV());
    _metadataOrTag = OperationWithoutOFCVTag{};
    _canPropagateAcrossShards = false;
}

BSONObj VersionContext::toBSON() const {
    return visit(
        OverloadedVisitor{[](const VersionContextMetadata& metadata) { return metadata.toBSON(); },
                          [](auto&&) -> BSONObj {
                              MONGO_UNREACHABLE_TASSERT(10083532);
                          }},
        _metadataOrTag);
}

bool VersionContext::_isMatchingOFCV(FCV fcv) const {
    return std::holds_alternative<VersionContextMetadata>(_metadataOrTag) &&
        std::get<VersionContextMetadata>(_metadataOrTag).getOFCV() == fcv;
}

void VersionContext::_assertHasNoOperationFCV() const {
    uassert(ErrorCodes::AlreadyInitialized,
            "The operation FCV has already been set.",
            !hasOperationFCV());
}

bool operator==(const VersionContext& lhs, const VersionContext& rhs) {
    return visit(
        OverloadedVisitor{[](const VersionContextMetadata& lhsMetadata,
                             const VersionContextMetadata& rhsMetadata) {
                              return lhsMetadata.getOFCV() == rhsMetadata.getOFCV();
                          },
                          [](auto&& lhsMetadataOrTag, auto&& rhsMetadataOrTag) {
                              return std::is_same_v<std::decay_t<decltype(lhsMetadataOrTag)>,
                                                    std::decay_t<decltype(rhsMetadataOrTag)>>;
                          }},
        lhs._metadataOrTag,
        rhs._metadataOrTag);
}

}  // namespace mongo
