/**
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without xbeven the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <boost/scoped_ptr.hpp>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/s/batched_delete_document.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

    /**
     * This class represents the layout and content of a batched delete runCommand,
     * the request side.
     */
    class BatchedDeleteRequest : public BSONSerializable {
        MONGO_DISALLOW_COPYING(BatchedDeleteRequest);
    public:

        //
        // schema declarations
        //

        // Name used for the batched delete invocation.
        static const std::string BATCHED_DELETE_REQUEST;

        // Field names and types in the batched delete command type.
        static const BSONField<std::string> collName;
        static const BSONField<std::vector<BatchedDeleteDocument*> > deletes;
        static const BSONField<BSONObj> writeConcern;
        static const BSONField<bool> continueOnError;
        static const BSONField<ChunkVersion> shardVersion;
        static const BSONField<long long> session;

        //
        // construction / destruction
        //

        BatchedDeleteRequest();
        virtual ~BatchedDeleteRequest();

        /** Copies all the fields present in 'this' to 'other'. */
        void cloneTo(BatchedDeleteRequest* other) const;

        //
        // bson serializable interface implementation
        //

        virtual bool isValid(std::string* errMsg) const;
        virtual BSONObj toBSON() const;
        virtual bool parseBSON(const BSONObj& source, std::string* errMsg);
        virtual void clear();
        virtual std::string toString() const;

        //
        // individual field accessors
        //

        void setCollName(const StringData& collName);
        void unsetCollName();
        bool isCollNameSet() const;
        const std::string& getCollName() const;

        void setDeletes(const std::vector<BatchedDeleteDocument*>& deletes);
        void addToDeletes(BatchedDeleteDocument* deletes);
        void unsetDeletes();
        bool isDeletesSet() const;
        std::size_t sizeDeletes() const;
        const std::vector<BatchedDeleteDocument*>& getDeletes() const;
        const BatchedDeleteDocument* getDeletesAt(std::size_t pos) const;

        void setWriteConcern(const BSONObj& writeConcern);
        void unsetWriteConcern();
        bool isWriteConcernSet() const;
        const BSONObj& getWriteConcern() const;

        void setContinueOnError(bool continueOnError);
        void unsetContinueOnError();
        bool isContinueOnErrorSet() const;
        bool getContinueOnError() const;

        void setShardVersion(const ChunkVersion& shardVersion);
        void unsetShardVersion();
        bool isShardVersionSet() const;
        const ChunkVersion& getShardVersion() const;

        void setSession(long long session);
        void unsetSession();
        bool isSessionSet() const;
        long long getSession() const;

    private:
        // Convention: (M)andatory, (O)ptional

        // (M)  collection we're deleting from
        std::string _collName;
        bool _isCollNameSet;

        // (M)  array of individual deletes
        std::vector<BatchedDeleteDocument*> _deletes;
        bool _isDeletesSet;

        // (M)  to be issued after the batch applied
        BSONObj _writeConcern;
        bool _isWriteConcernSet;

        // (M)  whether batch is issued in parallel or not
        bool _continueOnError;
        bool _isContinueOnErrorSet;

        // (O)  version for this collection on a given shard
        boost::scoped_ptr<ChunkVersion> _shardVersion;

        // (O)  session number the inserts belong to
        long long _session;
        bool _isSessionSet;
    };

} // namespace mongo
