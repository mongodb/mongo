/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/logv2/log.h"
#include "mongo/util/progress_meter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {

extern FailPoint hangIndexBuildDuringBulkLoadPhase;
extern FailPoint hangIndexBuildDuringBulkLoadPhaseSecond;

/* BulkBuilderCommon uses CRTP to implement a generic loop for draining keys from a bulk builder.
 * Child classes must implement these functions.
 *
 *   Return inserter that will insert keys into the index
 *   Also must initialize _ns to the namespace string.
 *   Inserter setUpBulkInserter(OperationContext* opCtx, bool dupsAllowed)
 *
 *   Return iterator it of the sorted keys for the type of the child class
 *   Iterator finalizeSort()
 *
 *   Check that current key comes after previous key in sort order.
 *   void debugEnsureSorted(const Key& data)
 *
 *   Return true if data is a duplicate, false otherwise. If duplicate checks don't apply, return
 *   false.
 *   bool duplicateCheck(OperationContext* opCtx, const Key& data, bool dupsAllowed,
 *                       const RecordIdHandlerFn& onDuplicateRecord)
 *
 *   Output key to write cursor.
 *   void insertKey(Inserter& inserter, const Key& data)
 *
 *  Perform finalizing steps for key.
 *  Status keyCommited(const KeyHandlerFn& onDuplicateKeyInserted, const Key& data, bool isDup)
 */
template <class T>
class BulkBuilderCommon : public IndexAccessMethod::BulkBuilder {

public:
    using KeyHandlerFn = std::function<Status(const key_string::Value&)>;
    using RecordIdHandlerFn = std::function<Status(const RecordId&)>;
    BulkBuilderCommon(int64_t numKeys, std::string message, std::string indexName)
        : _keysInserted(numKeys), _progressMessage(message), _indexName(indexName){};

    Status commit(OperationContext* opCtx,
                  const CollectionPtr& collection,
                  const IndexCatalogEntry* entry,
                  bool dupsAllowed,
                  int32_t yieldIterations,
                  const KeyHandlerFn& onDuplicateKeyInserted,
                  const RecordIdHandlerFn& onDuplicateRecord) {

        Timer timer;

        auto builder = static_cast<T*>(this)->setUpBulkInserter(opCtx, entry, dupsAllowed);
        auto it = static_cast<T*>(this)->finalizeSort();

        ProgressMeterHolder pm;
        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            pm.set(lk,
                   CurOp::get(opCtx)->setProgress_inlock(
                       _progressMessage, _keysInserted, 3 /* secondsBetween */),
                   opCtx);
        }  // namespace mongo

        int64_t iterations = 0;
        while (it->more()) {
            opCtx->checkForInterrupt();

            auto failPointHang = [opCtx, iterations, &indexName = _indexName](FailPoint* fp) {
                fp->executeIf(
                    [fp, opCtx, iterations, &indexName](const BSONObj& data) {
                        LOGV2(4924400,
                              "Hanging index build during bulk load phase",
                              "iteration"_attr = iterations,
                              "index"_attr = indexName);

                        fp->pauseWhileSet(opCtx);
                    },
                    [iterations, &indexName](const BSONObj& data) {
                        auto indexNames = data.getObjectField("indexNames");
                        return iterations == data["iteration"].numberLong() &&
                            std::any_of(indexNames.begin(),
                                        indexNames.end(),
                                        [&indexName](const auto& elem) {
                                            return indexName == elem.String();
                                        });
                    });
            };
            failPointHang(&hangIndexBuildDuringBulkLoadPhase);
            failPointHang(&hangIndexBuildDuringBulkLoadPhaseSecond);

            auto data = it->next();
            if (kDebugBuild) {
                static_cast<T*>(this)->debugEnsureSorted(data);
            }

            // Before attempting to insert, perform a duplicate key check.
            bool isDup;
            try {
                isDup = static_cast<T*>(this)->duplicateCheck(
                    opCtx, entry, data, dupsAllowed, onDuplicateRecord);
            } catch (DBException& e) {
                return e.toStatus();
            }

            if (isDup && !dupsAllowed) {
                continue;
            }


            try {
                writeConflictRetry(opCtx, "addingKey", _ns, [&] {
                    WriteUnitOfWork wunit(opCtx);
                    static_cast<T*>(this)->insertKey(builder, data);
                    wunit.commit();
                });
            } catch (DBException& e) {
                Status status = e.toStatus();
                // Duplicates are checked before inserting.
                invariant(status.code() != ErrorCodes::DuplicateKey);
                return status;
            }

            Status status =
                static_cast<T*>(this)->keyCommitted(onDuplicateKeyInserted, data, isDup);
            if (!status.isOK())
                return status;

            // Yield locks every 'yieldIterations' key insertions.
            if (yieldIterations > 0 && (++iterations % yieldIterations == 0)) {
                entry = yield(opCtx, collection, _ns, entry);
            }

            {
                stdx::unique_lock<Client> lk(*opCtx->getClient());
                // If we're here either it's a dup and we're cool with it or the addKey went just
                // fine.
                pm.get(lk)->hit();
            }
        }

        {
            stdx::unique_lock<Client> lk(*opCtx->getClient());
            pm.get(lk)->finished();
        }

        LOGV2(20685,
              "Index build: inserted {bulk_getKeysInserted} keys from external sorter into "
              "index in "
              "{timer_seconds} seconds",
              "Index build: inserted keys from external sorter into index",
              logAttrs(_ns),
              "index"_attr = _indexName,
              "keysInserted"_attr = _keysInserted,
              "duration"_attr = duration_cast<Milliseconds>(timer.elapsed()));
        return Status::OK();
    }

protected:
    int64_t _keysInserted = 0;
    std::string _progressMessage;
    std::string _indexName;
    NamespaceString _ns;
};
};  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
