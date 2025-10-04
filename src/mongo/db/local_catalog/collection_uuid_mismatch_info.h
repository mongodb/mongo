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

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
class CollectionUUIDMismatchInfo final : public ErrorExtraInfo {
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
