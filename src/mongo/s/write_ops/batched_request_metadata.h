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

#include <string>

#include "mongo/bson/bson_field.h"
#include "mongo/base/disallow_copying.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/chunk_version.h"

namespace mongo {
    class BatchedRequestMetadata : public BSONSerializable {
        MONGO_DISALLOW_COPYING(BatchedRequestMetadata);
    public:

        static const BSONField<std::string> shardName;
        static const BSONField<ChunkVersion> shardVersion;
        static const BSONField<long long> session;

        BatchedRequestMetadata();
        virtual ~BatchedRequestMetadata();

        //
        // bson serializable interface implementation
        //

        virtual bool isValid(std::string* errMsg) const;
        virtual BSONObj toBSON() const;
        virtual bool parseBSON(const BSONObj& source, std::string* errMsg);
        virtual void clear();
        virtual std::string toString() const;

        void cloneTo(BatchedRequestMetadata* other) const;

        //
        // individual field accessors
        //

        void setShardName(const StringData& shardName);
        void unsetShardName();
        bool isShardNameSet() const;
        const std::string& getShardName() const;

        void setShardVersion(const ChunkVersion& shardVersion);
        void unsetShardVersion();
        bool isShardVersionSet() const;
        const ChunkVersion& getShardVersion() const;

        void setSession(long long session);
        void unsetSession();
        bool isSessionSet() const;
        long long getSession() const;

    private:

        // (O)  shard name we're sending this batch to
        std::string _shardName;
        bool _isShardNameSet;

        // (O)  version for this collection on a given shard
        boost::scoped_ptr<ChunkVersion> _shardVersion;

        // (O)  session number the inserts belong to
        long long _session;
        bool _isSessionSet;
    };
}
