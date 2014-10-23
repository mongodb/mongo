// kv_engine_impl.cpp

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

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/kv/dictionary/kv_engine_impl.h"
#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/kv/dictionary/kv_record_store.h"
#include "mongo/db/storage/kv/dictionary/kv_record_store_capped.h"
#include "mongo/db/storage/kv/dictionary/kv_sorted_data_impl.h"

namespace mongo {

    /**
     * @param ident Ident is a one time use string. It is used for this instance
     *              and never again.
     */
    Status KVEngineImpl::createRecordStore( OperationContext* opCtx,
                                            const StringData& ns,
                                            const StringData& ident,
                                            const CollectionOptions& options ) {
        // Creating a record store is as simple as creating one with the given `ident'
        return createKVDictionary(opCtx, ident, KVDictionary::Comparator::useMemcmp());
    }

    /**
     * Caller takes ownership
     * Having multiple out for the same ns is a rules violation;
     * Calling on a non-created ident is invalid and may crash.
     */
    RecordStore* KVEngineImpl::getRecordStore( OperationContext* opCtx,
                                               const StringData& ns,
                                               const StringData& ident,
                                               const CollectionOptions& options ) {
        auto_ptr<KVDictionary> db(getKVDictionary(opCtx, ident, KVDictionary::Comparator::useMemcmp()));
        auto_ptr<KVRecordStore> rs;
        // We separated the implementations of capped / non-capped record stores for readability.
        if (options.capped) {
            rs.reset(new KVRecordStoreCapped(db.release(), opCtx, ns, options));
        } else {
            rs.reset(new KVRecordStore(db.release(), opCtx, ns, options));
        }
        if (persistDictionaryStats()) {
            rs->setStatsMetadataDictionary(opCtx, getMetadataDictionary());
        }
        return rs.release();
    }

    Status KVEngineImpl::dropRecordStore( OperationContext* opCtx,
                                          const StringData& ident ) {
        if (persistDictionaryStats()) {
            KVRecordStore::deleteMetadataKeys(opCtx, getMetadataDictionary(), ident);
        }
        // Dropping a record store is as simple as dropping its underlying KVDictionary.
        return dropKVDictionary(opCtx, ident);
    }

    // --------

    Status KVEngineImpl::createSortedDataInterface( OperationContext* opCtx,
                                                    const StringData& ident,
                                                    const IndexDescriptor* desc ) {
        // Creating a sorted data impl is as simple as creating one with the given `ident'
        //
        // Assume the default ordering if no desc is passed (for tests)
        const BSONObj keyPattern = desc ? desc->keyPattern() : BSONObj();
        IndexEntryComparison cmp(Ordering::make(keyPattern));
        return createKVDictionary(opCtx, ident, KVDictionary::Comparator::useIndexEntryComparison(cmp));

    }

    SortedDataInterface* KVEngineImpl::getSortedDataInterface( OperationContext* opCtx,
                                                               const StringData& ident,
                                                               const IndexDescriptor* desc ) {
        // Assume the default ordering if no desc is passed (for tests)
        const BSONObj keyPattern = desc ? desc->keyPattern() : BSONObj();
        IndexEntryComparison cmp(Ordering::make(keyPattern));
        auto_ptr<KVDictionary> db(getKVDictionary(opCtx, ident, KVDictionary::Comparator::useIndexEntryComparison(cmp)));
        return new KVSortedDataImpl(db.release(), opCtx, desc);
    }

    Status KVEngineImpl::dropSortedDataInterface( OperationContext* opCtx,
                                                  const StringData& ident ) {
        // Dropping a sorted data impl is as simple as dropping its underlying KVDictionary
        return dropKVDictionary(opCtx, ident);
    }

}
