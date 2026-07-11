// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] CollectionUUIDMismatchInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::CollectionUUIDMismatch;

    explicit CollectionUUIDMismatchInfo(DatabaseName dbName,
                                        UUID collectionUUID,
                                        std::string expectedCollection,
                                        boost::optional<std::string> actualCollection)
        : _dbName(std::move(dbName)),
          _collectionUUID(std::move(collectionUUID)),
          _expectedCollection(std::move(expectedCollection)),
          _actualCollection(std::move(actualCollection)) {}

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    void serialize(BSONObjBuilder* builder) const override;

    const auto& dbName() const {
        return _dbName;
    }

    const auto& collectionUUID() const {
        return _collectionUUID;
    }

    const auto& expectedCollection() const {
        return _expectedCollection;
    }

    const auto& actualCollection() const {
        return _actualCollection;
    }

private:
    DatabaseName _dbName;
    UUID _collectionUUID;
    std::string _expectedCollection;
    boost::optional<std::string> _actualCollection;
};

}  // namespace mongo
