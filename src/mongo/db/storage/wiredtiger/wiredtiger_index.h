/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <boost/shared_ptr.hpp>
#include <wiredtiger.h>

#include "mongo/base/status_with.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

namespace mongo {

    class IndexCatalogEntry;
    class IndexDescriptor;
    struct WiredTigerItem;

    class WiredTigerIndex : public SortedDataInterface {
    public:

        /**
         * Parses index options for wired tiger configuration string suitable for table creation.
         * The document 'options' is typically obtained from the 'storageEngine.wiredTiger' field
         * of an IndexDescriptor's info object.
         */
        static StatusWith<std::string> parseIndexOptions(const BSONObj& options);

        /**
         * Creates a configuration string suitable for 'config' parameter in WT_SESSION::create().
         * Configuration string is constructed from:
         *     built-in defaults
         *     'extraConfig'
         *     storageEngine.wiredTiger.configString in index descriptor's info object.
         * Performs simple validation on the supplied parameters.
         * Returns error status if validation fails.
         * Note that even if this function returns an OK status, WT_SESSION:create() may still
         * fail with the constructed configuration string.
         */
        static StatusWith<std::string> generateCreateString(const std::string& extraConfig,
                                                            const IndexDescriptor& desc);

        /**
         * Creates a WiredTiger table suitable for implementing a MongoDB index.
         * 'config' should be created with generateCreateString().
         */
        static int Create(OperationContext* txn,
                          const std::string& uri,
                          const std::string& config);

        /**
         * @param unique - If this is a unique index.
         *                 Note: even if unique, it may be allowed ot be non-unique at times.
         */
        WiredTigerIndex(const std::string& uri, const IndexDescriptor* desc);

        virtual SortedDataBuilderInterface* getBulkBuilder(OperationContext* txn, bool dupsAllowed);

        virtual Status insert(OperationContext* txn,
                              const BSONObj& key,
                              const RecordId& loc,
                              bool dupsAllowed);

        virtual void unindex(OperationContext* txn,
                             const BSONObj& key,
                             const RecordId& loc,
                             bool dupsAllowed);

        virtual void fullValidate(OperationContext* txn, bool full, long long *numKeysOut,
                                  BSONObjBuilder* output) const;

        virtual Status dupKeyCheck(OperationContext* txn, const BSONObj& key, const RecordId& loc);

        virtual bool isEmpty(OperationContext* txn);

        virtual long long getSpaceUsedBytes( OperationContext* txn ) const;

        bool isDup(WT_CURSOR *c, const BSONObj& key, const RecordId& loc );

        virtual SortedDataInterface::Cursor* newCursor(
                                                       OperationContext* txn, int direction) const;

        virtual Status initAsEmpty(OperationContext* txn);

        const std::string& uri() const { return _uri; }

        uint64_t instanceId() const { return _instanceId; }

        virtual bool unique() const = 0;

    protected:

        virtual Status _insert( WT_CURSOR* c,
                                const BSONObj& key,
                                const RecordId& loc,
                                bool dupsAllowed ) = 0;

        virtual void _unindex( WT_CURSOR* c,
                               const BSONObj& key,
                               const RecordId& loc,
                               bool dupsAllowed ) = 0;

        class BulkBuilder;
        class StandardBulkBuilder;
        class UniqueBulkBuilder;

        class IndexCursor : public SortedDataInterface::Cursor {
        public:
            IndexCursor(const WiredTigerIndex& idx,
                        OperationContext *txn,
                        bool forward);

            virtual ~IndexCursor() { }

            virtual int getDirection() const { return _forward ? 1 : -1; }

            virtual bool isEOF() const { return _eof; }

            virtual bool pointsToSamePlaceAs(const SortedDataInterface::Cursor &genother) const;

            virtual void aboutToDeleteBucket(const RecordId& bucket);

            virtual bool locate(const BSONObj &key, const RecordId& loc);

            virtual void customLocate(const BSONObj& keyBegin,
                                      int keyBeginLen,
                                      bool afterKey,
                                      const vector<const BSONElement*>& keyEnd,
                                      const vector<bool>& keyEndInclusive);

            void advanceTo(const BSONObj &keyBegin,
                           int keyBeginLen,
                           bool afterKey,
                           const vector<const BSONElement*>& keyEnd,
                           const vector<bool>& keyEndInclusive);

            virtual BSONObj getKey() const;

            virtual RecordId getRecordId() const;

            virtual void advance();

            virtual void savePosition();

            virtual void restorePosition( OperationContext *txn );

        private:
            bool _locate(const BSONObj &key, const RecordId& loc);

            OperationContext *_txn;
            WiredTigerCursor _cursor;
            const WiredTigerIndex& _idx; // not owned
            bool _forward;
            bool _eof;

            mutable int _uniquePos;
            mutable int _uniqueLen;

            // For save/restorePosition check
            RecoveryUnit* _savedForCheck;
            BSONObj _savedKey;
            RecordId _savedLoc;
        };

        const Ordering _ordering;
        std::string _uri;
        uint64_t _instanceId;
    };


    class WiredTigerIndexUnique : public WiredTigerIndex {
    public:
        WiredTigerIndexUnique( const std::string& uri, const IndexDescriptor* desc );

        virtual bool unique() const { return true; }

        virtual Status _insert( WT_CURSOR* c,
                                const BSONObj& key,
                                const RecordId& loc,
                                bool dupsAllowed );

        virtual void _unindex( WT_CURSOR* c,
                               const BSONObj& key,
                               const RecordId& loc,
                               bool dupsAllowed );
    };

    class WiredTigerIndexStandard : public WiredTigerIndex {
    public:
        WiredTigerIndexStandard( const std::string& uri, const IndexDescriptor* desc );

        virtual bool unique() const { return false; }

        virtual Status _insert( WT_CURSOR* c,
                                const BSONObj& key,
                                const RecordId& loc,
                                bool dupsAllowed );

        virtual void _unindex( WT_CURSOR* c,
                               const BSONObj& key,
                               const RecordId& loc,
                               bool dupsAllowed );

    };

} // namespace
