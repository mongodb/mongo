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
#include "mongo/s/write_ops/batched_insert_request.h"
#include "mongo/s/write_ops/batched_update_request.h"
#include "mongo/s/write_ops/batched_delete_request.h"

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

        /**
         * insertReq ownership is transferred to here.
         */
        BatchedCommandRequest( BatchedInsertRequest* insertReq ) :
                _batchType( BatchType_Insert ), _insertReq( insertReq ) {
        }

        /**
         * updateReq ownership is transferred to here.
         */
        BatchedCommandRequest( BatchedUpdateRequest* updateReq ) :
                _batchType( BatchType_Update ), _updateReq( updateReq ) {
        }

        /**
         * deleteReq ownership is transferred to here.
         */
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
        // Batch type accessors
        //

        BatchType getBatchType() const;
        BatchedInsertRequest* getInsertRequest() const;
        BatchedUpdateRequest* getUpdateRequest() const;
        BatchedDeleteRequest* getDeleteRequest() const;
        // Index creation is also an insert, but a weird one.
        bool isInsertIndexRequest() const;
        bool isUniqueIndexRequest() const;
        std::string getTargetingNS() const;
        BSONObj getIndexKeyPattern() const;

        //
        // individual field accessors
        //

        bool isVerboseWC() const;

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
        std::size_t sizeWriteOps() const;
        std::vector<BSONObj> getWriteOps() const;

        void setWriteConcern( const BSONObj& writeConcern );
        void unsetWriteConcern();
        bool isWriteConcernSet() const;
        const BSONObj& getWriteConcern() const;

        void setOrdered( bool ordered );
        void unsetOrdered();
        bool isOrderedSet() const;
        bool getOrdered() const;

        BatchedRequestMetadata* getMetadata() const;
        void setMetadata(BatchedRequestMetadata* metadata);

        //
        // Helpers for auth pre-parsing
        //

        /**
         * Helper to determine whether or not there are any upserts in the batch
         */
        static bool containsUpserts( const BSONObj& writeCmdObj );

        /**
         * Helper to extract the namespace being indexed from a raw BSON write command.
         *
         * Returns false with errMsg if the index write command seems invalid.
         * TODO: Remove when we have parsing hooked before authorization
         */
        static bool getIndexedNS( const BSONObj& writeCmdObj,
                                  std::string* nsToIndex,
                                  std::string* errMsg );

    private:

        BatchType _batchType;
        scoped_ptr<BatchedInsertRequest> _insertReq;
        scoped_ptr<BatchedUpdateRequest> _updateReq;
        scoped_ptr<BatchedDeleteRequest> _deleteReq;

    };

    /**
     * Similar to above, this class wraps the write items of a command request into a generically
     * usable type.  Very thin wrapper, does not own the write item itself.
     *
     * TODO: Use in BatchedCommandRequest above
     */
    class BatchItemRef {
    public:

        BatchItemRef( const BatchedCommandRequest* request, int itemIndex ) :
            _request( request ), _itemIndex( itemIndex ) {
            dassert( itemIndex < static_cast<int>( request->sizeWriteOps() ) );
        }

        const BatchedCommandRequest* getRequest() const {
            return _request;
        }

        int getItemIndex() const {
            return _itemIndex;
        }

        BatchedCommandRequest::BatchType getOpType() const {
            return _request->getBatchType();
        }

        BSONObj getDocument() const {
            return _request->getInsertRequest()->getDocumentsAt( _itemIndex );
        }

        const BatchedUpdateDocument* getUpdate() const {
            return _request->getUpdateRequest()->getUpdatesAt( _itemIndex );
        }

        const BatchedDeleteDocument* getDelete() const {
            return _request->getDeleteRequest()->getDeletesAt( _itemIndex );
        }

    private:

        const BatchedCommandRequest* _request;
        const int _itemIndex;
    };

} // namespace mongo
