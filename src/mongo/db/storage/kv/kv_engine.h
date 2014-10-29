// kv_engine.h

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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"

namespace mongo {

    class IndexDescriptor;
    class OperationContext;
    class RecordStore;
    class RecoveryUnit;
    class SortedDataInterface;

    class KVEngine {
    public:

        virtual ~KVEngine() {}

        virtual RecoveryUnit* newRecoveryUnit() = 0;

        // ---------

        /**
         * Caller takes ownership
         * Having multiple out for the same ns is a rules violation;
         * Calling on a non-created ident is invalid and may crash.
         */
        virtual RecordStore* getRecordStore( OperationContext* opCtx,
                                             const StringData& ns,
                                             const StringData& ident,
                                             const CollectionOptions& options ) = 0;

        virtual SortedDataInterface* getSortedDataInterface( OperationContext* opCtx,
                                                             const StringData& ident,
                                                             const IndexDescriptor* desc ) = 0;

        //
        // The create and drop methods on KVEngine are not transactional. Transactional semantics
        // are provided by the KVStorageEngine code that calls these. For example, drop will be
        // called if a create is rolled back. A higher-level drop operation will only propagate to a
        // drop call on the KVEngine once the WUOW commits. Therefore drops will never be rolled
        // back and it is safe to immediately reclaim storage.
        //

        virtual Status createRecordStore( OperationContext* opCtx,
                                          const StringData& ns,
                                          const StringData& ident,
                                          const CollectionOptions& options ) = 0;
        virtual Status dropRecordStore( OperationContext* opCtx,
                                        const StringData& ident ) = 0;


        virtual Status createSortedDataInterface( OperationContext* opCtx,
                                                  const StringData& ident,
                                                  const IndexDescriptor* desc ) = 0;

        virtual Status dropSortedDataInterface( OperationContext* opCtx,
                                                const StringData& ident ) = 0;

        // optional
        virtual int flushAllFiles( bool sync ) { return 0; }

        /**
         * This must not change over the lifetime of the engine.
         */
        virtual bool supportsDocLocking() const = 0;

        virtual Status okToRename( OperationContext* opCtx,
                                   const StringData& fromNS,
                                   const StringData& toNS,
                                   const StringData& ident,
                                   const RecordStore* originalRecordStore ) const {
            return Status::OK();
        }
    };

}
