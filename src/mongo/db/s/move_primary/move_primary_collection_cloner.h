/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include <memory>
#include <vector>

#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/db/s/move_primary/move_primary_base_cloner.h"
#include "mongo/db/s/move_primary/move_primary_shared_data.h"
#include "mongo/util/progress_meter.h"

namespace mongo {

class MovePrimaryCollectionCloner : public MovePrimaryBaseCloner {
public:
    struct Stats {
        static constexpr StringData kDocumentsToCopyFieldName = "documentsToCopy"_sd;
        static constexpr StringData kDocumentsCopiedFieldName = "documentsCopied"_sd;

        std::string ns;
        Date_t start;
        Date_t end;
        size_t documentToCopy{0};
        size_t documentsCopied{0};
        size_t indexes{0};
        size_t insertedBatches{0};
        size_t receivedBatches{0};
        long long avgObjSize{0};
        long long approxTotalDataSize{0};
        long long approxTotalBytesCopied{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    MovePrimaryCollectionCloner(const CollectionParams& collectionParams,
                                MovePrimarySharedData* sharedData,
                                const HostAndPort& source,
                                DBClientConnection* client,
                                repl::StorageInterface* storageInterface,
                                ThreadPool* dbPool);

    virtual ~MovePrimaryCollectionCloner() = default;

    Stats getStats() const;

protected:
    ClonerStages getStages() final;

private:
    friend class MovePrimaryCollectionClonerTest;
    friend class MovePrimaryCollectionClonerStage;

    class MovePrimaryCollectionClonerStage : public ClonerStage<MovePrimaryCollectionCloner> {
    public:
        MovePrimaryCollectionClonerStage(std::string name,
                                         MovePrimaryCollectionCloner* cloner,
                                         ClonerRunFn stageFunc)
            : ClonerStage<MovePrimaryCollectionCloner>(name, cloner, stageFunc) {}
        AfterStageBehavior run() override;

        bool isTransientError(const Status& status) override {
            // Always abort on error.
            return false;
        }
    };

    /**
     * The preStage sets the start time in _stats.
     */
    void preStage() final;

    /**
     * The postStage sets the end time in _stats.
     */
    void postStage() final;

    /**
     * Stage function that counts the number of documents in the collection on the source in order
     * to generate progress information.
     */
    AfterStageBehavior countStage();

    /**
     * Stage function that checks to see if the donor collection is empty (and therefore we may
     * race with createIndexes on empty collections) before running listIndexes.
     */
    AfterStageBehavior checkIfDonorCollectionIsEmptyStage();

    /**
     * Stage function that gets the index information of the collection on the source to re-create
     * it.
     */
    AfterStageBehavior listIndexesStage();

    /**
     * Stage function that creates the collection using the storageInterface.  This stage does not
     * actually contact the sync source.
     */
    AfterStageBehavior createCollectionStage();

    /**
     * Stage function that executes a query to retrieve all documents in the collection.  For each
     * batch returned by the upstream node, handleNextBatch will be called with the data.  This
     * stage will finish when the entire query is finished or failed.
     */
    AfterStageBehavior queryStage();

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    const CollectionParams _collectionParams;                              // (R)
    MovePrimaryCollectionClonerStage _countStage;                          // (R)
    MovePrimaryCollectionClonerStage _checkIfDonorCollectionIsEmptyStage;  // (R)
    MovePrimaryCollectionClonerStage _listIndexesStage;                    // (R)
    MovePrimaryCollectionClonerStage _createCollectionStage;               // (R)
    MovePrimaryCollectionClonerStage _queryStage;                          // (R)

    Stats _stats;  // (M)
};

}  // namespace mongo
