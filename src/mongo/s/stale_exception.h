/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class StaleConfigInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleConfig;

    StaleConfigInfo(const std::string& ns, ChunkVersion received, ChunkVersion wanted)
        : _ns(ns), _received(received), _wanted(wanted) {}

    StaleConfigInfo(const BSONObj& commandError);

    StaleConfigInfo() = default;

    std::string getns() const {
        return _ns;
    }

    ChunkVersion getVersionReceived() const {
        return _received;
    }

    ChunkVersion getVersionWanted() const {
        return _wanted;
    }

    /**
     * Returns true if this exception would require a full reload of config data to resolve.
     */
    bool requiresFullReload() const {
        return !_received.hasEqualEpoch(_wanted) || _received.isSet() != _wanted.isSet();
    }

    void serialize(BSONObjBuilder* bob) const final;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

private:
    std::string _ns;
    ChunkVersion _received;
    ChunkVersion _wanted;
};

class StaleDbRoutingVersion final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleDbVersion;

    StaleDbRoutingVersion(const std::string& db,
                          DatabaseVersion received,
                          boost::optional<DatabaseVersion> wanted)
        : _db(db), _received(received), _wanted(wanted) {}

    StaleDbRoutingVersion(const BSONObj& commandError);

    StaleDbRoutingVersion() = default;

    const std::string& getDb() const {
        return _db;
    }

    DatabaseVersion getVersionReceived() const {
        return _received;
    }

    boost::optional<DatabaseVersion> getVersionWanted() const {
        return _wanted;
    }

    void serialize(BSONObjBuilder* bob) const final;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

private:
    std::string _db;
    DatabaseVersion _received;
    boost::optional<DatabaseVersion> _wanted;
};

using StaleConfigException = ExceptionFor<ErrorCodes::StaleConfig>;

}  // namespace mongo
