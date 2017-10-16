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
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

/**
 * Thrown whenever the config info for a given shard/chunk is out of date.
 */
class StaleConfigException final : public AssertionException {
public:
    StaleConfigException(const std::string& ns,
                         const std::string& raw,
                         ChunkVersion received,
                         ChunkVersion wanted)
        : AssertionException(ErrorCodes::StaleConfig,
                             str::stream() << raw << " ( ns : " << ns << ", received : "
                                           << received.toString()
                                           << ", wanted : "
                                           << wanted.toString()
                                           << " )"),
          _ns(ns),
          _received(received),
          _wanted(wanted) {}

    /** Preferred if we're rebuilding this from a thrown exception */
    StaleConfigException(const std::string& raw, const BSONObj& error)
        : AssertionException(ErrorCodes::StaleConfig,
                             str::stream() << raw << " ( ns : " << (error["ns"].type() == String
                                                                        ? error["ns"].String()
                                                                        : std::string("<unknown>"))
                                           << ", received : "
                                           << ChunkVersion::fromBSON(error, "vReceived").toString()
                                           << ", wanted : "
                                           << ChunkVersion::fromBSON(error, "vWanted").toString()
                                           << " )"),
          // For legacy reasons, we may not always get a namespace here
          _ns(error["ns"].type() == String ? error["ns"].String() : ""),
          _received(ChunkVersion::fromBSON(error, "vReceived")),
          _wanted(ChunkVersion::fromBSON(error, "vWanted")) {}

    /**
     * TODO: This constructor is only necessary, because ParallelSortClusteredCursor puts per-host
     * stale config exceptions in a map and this requires a default constructor.
     */
    StaleConfigException()
        : AssertionException(ErrorCodes::InternalError,
                             "initializing empty stale config exception object") {}

    virtual ~StaleConfigException() throw() {}

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

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}

    std::string _ns;
    ChunkVersion _received;
    ChunkVersion _wanted;
};

}  // namespace mongo
