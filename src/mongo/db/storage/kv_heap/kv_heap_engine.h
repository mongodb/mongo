// kv_heap_engine.h

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

#include "mongo/db/storage/kv/dictionary/kv_engine_impl.h"

namespace mongo {

    class KVHeapEngine : public KVEngineImpl {
    public:
        virtual ~KVHeapEngine() { }

        RecoveryUnit* newRecoveryUnit();

        Status createKVDictionary( OperationContext* opCtx,
                                    const StringData& ident,
                                    const KVDictionary::Comparator &cmp );

        KVDictionary* getKVDictionary( OperationContext* opCtx,
                                        const StringData& ident,
                                        const KVDictionary::Comparator &cmp,
                                        bool mayCreate = false );

        Status dropKVDictionary( OperationContext* opCtx,
                                  const StringData& ident );

        int64_t getIdentSize( OperationContext* opCtx,
                              const StringData& ident ) {
            return 1;
        }

        Status repairIdent( OperationContext* opCtx,
                            const StringData& ident ) {
            return Status::OK();
        }

        int flushAllFiles( bool sync ) { return 0; }

        bool isDurable() const { return false; }

        // THe KVDictionaryHeap does not support fine-grained locking.
        bool supportsDocLocking() const { return false; }

    };

}
