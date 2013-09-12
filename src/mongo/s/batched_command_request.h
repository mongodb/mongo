/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/s/bson_serializable.h"
#include "mongo/s/batched_insert_request.h"
#include "mongo/s/batched_update_request.h"
#include "mongo/s/batched_delete_request.h"

namespace mongo {

    /**
     * This class wraps the different kinds of command requests into a generically usable write
     * command request.
     *
     * Designed to be a very thin wrapper that mimics the underlying requests exactly.  Owns the
     * wrapped request object once constructed.
     */
    class BatchedCommandRequest : public BSONSerializable {
    MONGO_DISALLOW_COPYING(BatchedCommandRequest);
    public:

        enum BatchType {
            BatchType_Insert, BatchType_Update, BatchType_Delete, BatchType_Unknown
        };

        //
        // construction / destruction
        //

        BatchedCommandRequest( BatchType batchType );

        BatchedCommandRequest( BatchedInsertRequest* insertReq ) :
                _batchType( BatchType_Insert ), _insertReq( insertReq ) {
        }
        BatchedCommandRequest( BatchedUpdateRequest* updateReq ) :
                _batchType( BatchType_Update ), _updateReq( updateReq ) {
        }
        BatchedCommandRequest( BatchedDeleteRequest* deleteReq ) :
                _batchType( BatchType_Delete ), _deleteReq( deleteReq ) {
        }

        virtual ~BatchedCommandRequest() {};

        /** Copies all the fields present in 'this' to 'other'. */
        void cloneTo( BatchedCommandRequest* other ) const;

        //
        // bson serializable interface implementation
        //

        virtual bool isValid( std::string* errMsg ) const;
        virtual BSONObj toBSON() const;
        virtual bool parseBSON( const BSONObj& source, std::string* errMsg );
        virtual void clear();
        virtual std::string toString() const;

        //
        // individual field accessors
        //

        BatchType getBatchType() const;

        void setNS( const StringData& collName );
        void unsetNS();
        bool isNSSet() const;
        const std::string& getNS() const;

        /**
         * Write ops are BSONObjs, whose format depends on the type of request
         * TODO: Should be possible to further parse these ops generically if we come up with a
         * good scheme.
         */
        void setWriteOps( const std::vector<BSONObj>& writeOps );
        void unsetWriteOps();
        bool isWriteOpsSet() const;
        std::vector<BSONObj> getWriteOps() const;

        void setWriteConcern( const BSONObj& writeConcern );
        void unsetWriteConcern();
        bool isWriteConcernSet() const;
        const BSONObj& getWriteConcern() const;

        void setContinueOnError( bool continueOnError );
        void unsetContinueOnError();
        bool isContinueOnErrorSet() const;
        bool getContinueOnError() const;

        void setShardVersion( const ChunkVersion& shardVersion );
        void unsetShardVersion();
        bool isShardVersionSet() const;
        const ChunkVersion& getShardVersion() const;

        void setSession( long long session );
        void unsetSession();
        bool isSessionSet() const;
        long long getSession() const;

    private:

        BatchType _batchType;
        scoped_ptr<BatchedInsertRequest> _insertReq;
        scoped_ptr<BatchedUpdateRequest> _updateReq;
        scoped_ptr<BatchedDeleteRequest> _deleteReq;

    };

} // namespace mongo
