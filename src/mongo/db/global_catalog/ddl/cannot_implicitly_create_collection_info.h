// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/modules.h"

#include <memory>
#include <utility>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] CannotImplicitlyCreateCollectionInfo final
    : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::CannotImplicitlyCreateCollection;

    CannotImplicitlyCreateCollectionInfo(NamespaceString nss) : _nss(std::move(nss)) {}

    const auto& getNss() const {
        return _nss;
    }

    void serialize(BSONObjBuilder* bob) const final;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

private:
    NamespaceString _nss;
};

}  // namespace mongo
