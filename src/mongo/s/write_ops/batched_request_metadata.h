/**
 *    Copyright (C) 2013 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/db/jsobj.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

class BatchedRequestMetadata {
public:
    static const BSONField<ChunkVersion> shardVersion;
    static const BSONField<long long> session;

    BatchedRequestMetadata();
    ~BatchedRequestMetadata();

    //
    // bson serializable interface implementation
    //

    bool isValid(std::string* errMsg) const;
    BSONObj toBSON() const;
    bool parseBSON(const BSONObj& source, std::string* errMsg);
    void clear();
    std::string toString() const;

    void cloneTo(BatchedRequestMetadata* other) const;

    void setShardVersion(const ChunkVersionAndOpTime& shardVersion);
    bool isShardVersionSet() const;
    const ChunkVersion& getShardVersion() const;
    const repl::OpTime& getOpTime() const;

    void setSession(long long session);
    void unsetSession();
    bool isSessionSet() const;
    long long getSession() const;

private:
    // (O)  version for this collection on a given shard
    boost::optional<ChunkVersionAndOpTime> _shardVersion;

    // (O)  session number the inserts belong to
    long long _session;
    bool _isSessionSet;
};

}  // namespace mongo
