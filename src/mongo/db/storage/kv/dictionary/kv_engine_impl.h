// kv_engine_impl.h

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

#include <boost/thread/mutex.hpp>

#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/kv/kv_engine.h"

namespace mongo {

    /*
     * A KVEngine interface that provides implementations for each of
     * create, get, and drop recordStore/sortedDataInterface that use
     * classes built on top of KVDictionary.
     *
     * Storage engine authors that have access to a sorted kv store API
     * are likely going to want to use this interface for KVEngine as it
     * only requires them to implement a subclass of KVDictionary (and a
     * recovery unit) and nothing more.
     */
    class KVEngineImpl : public KVEngine {
    public:
        virtual ~KVEngineImpl() { }

        virtual RecoveryUnit* newRecoveryUnit() = 0;

        // ---------

        /**
         * @param ident Ident is a one time use string. It is used for this instance
         *              and never again.
         */
        Status createRecordStore( OperationContext* opCtx,
                                  const StringData& ns,
                                  const StringData& ident,
                                  const CollectionOptions& options );

        /**
         * Caller takes ownership
         * Having multiple out for the same ns is a rules violation;
         * Calling on a non-created ident is invalid and may crash.
         */
        RecordStore* getRecordStore( OperationContext* opCtx,
                                     const StringData& ns,
                                     const StringData& ident,
                                     const CollectionOptions& options );

        Status dropRecordStore( OperationContext* opCtx,
                                const StringData& ident );

        // --------

        Status createSortedDataInterface( OperationContext* opCtx,
                                          const StringData& ident,
                                          const IndexDescriptor* desc );

        SortedDataInterface* getSortedDataInterface( OperationContext* opCtx,
                                                     const StringData& ident,
                                                     const IndexDescriptor* desc );

        Status dropSortedDataInterface( OperationContext* opCtx,
                                        const StringData& ident );
    protected:
        // Create a KVDictionary (same rules as createRecordStore / createSortedDataInterface)
        // 
        // param: cmp, the comparator that should be passed to the KVDictionary
        virtual Status createKVDictionary( OperationContext* opCtx,
                                           const StringData& ident,
                                           const KVDictionary::Comparator &cmp ) = 0;

        // Get a KVDictionary (same rules as getRecordStore / getSortedDataInterface)
        //
        // param: cmp, the comparator that should be passed to the KVDictionary
        virtual KVDictionary* getKVDictionary( OperationContext* opCtx,
                                               const StringData& ident,
                                               const KVDictionary::Comparator &cmp,
                                               bool mayCreate = false ) = 0;

        // Drop a KVDictionary (same rules as dropRecordStore / dropSortedDataInterface)
        virtual Status dropKVDictionary( OperationContext* opCtx,
                                         const StringData& ident ) = 0;
    };

}
