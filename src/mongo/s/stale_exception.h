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

#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version_gen.h"

namespace mongo {

class StaleConfigInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleConfig;

    StaleConfigInfo(NamespaceString nss,
                    ChunkVersion received,
                    boost::optional<ChunkVersion> wanted)
        : _nss(std::move(nss)), _received(received), _wanted(wanted) {}

    const auto& getNss() const {
        return _nss;
    }

    const auto& getVersionReceived() const {
        return _received;
    }

    const auto& getVersionWanted() const {
        return _wanted;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);
    static StaleConfigInfo parseFromCommandError(const BSONObj& commandError);

private:
    NamespaceString _nss;
    ChunkVersion _received;
    boost::optional<ChunkVersion> _wanted;
};
using StaleConfigException = ExceptionFor<ErrorCodes::StaleConfig>;

class StaleDbRoutingVersion final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleDbVersion;

    StaleDbRoutingVersion(std::string db,
                          DatabaseVersion received,
                          boost::optional<DatabaseVersion> wanted)
        : _db(std::move(db)), _received(received), _wanted(wanted) {}

    const auto& getDb() const {
        return _db;
    }

    const auto& getVersionReceived() const {
        return _received;
    }

    const auto& getVersionWanted() const {
        return _wanted;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);
    static StaleDbRoutingVersion parseFromCommandError(const BSONObj& commandError);

private:
    std::string _db;
    DatabaseVersion _received;
    boost::optional<DatabaseVersion> _wanted;
};

}  // namespace mongo
