/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/rpc/metadata/audit_metadata.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * This is a base abstract class for all stages performing a write operation into an output
 * collection. The writes are organized in batches in which elements are objects of the templated
 * type 'B'. A subclass must override the following methods to be able to write into the output
 * collection:
 *
 *    - 'makeBatchObject()' - creates an object of type 'B' from the given 'Document', which is,
 *       essentially, a result of the input source's 'getNext()' .
 *    - 'flush()' - writes the batch into the output collection.
 *    - 'makeBatchedWriteRequest()' - initializes the request object for writing a batch to
 *       the output collection.
 *
 * Two other virtual methods exist which a subclass may override: 'initialize()' and 'finalize()',
 * which are called before the first element is read from the input source, and after the last one
 * has been read, respectively.
 */
template <typename B>
class WriterStage : public Stage {

public:
    using BatchObject = B;
    using BatchedObjects = std::vector<BatchObject>;
    WriterStage(StringData stageName,
                const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                NamespaceString outputNs)
        : Stage(stageName, pExpCtx),
          _writeSizeEstimator(pExpCtx->getMongoProcessInterface()->getWriteSizeEstimator(
              pExpCtx->getOperationContext(), outputNs)),
          _outputNs(std::move(outputNs)) {};

protected:
    GetNextResult doGetNext() override {
        if (_done) {
            return GetNextResult::makeEOF();
        }

        // Ignore writes and exhaust input if we are in explain mode.
        if (pExpCtx->getExplain()) {
            auto nextInput = pSource->getNext();
            for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
            }
            _done = nextInput.getStatus() == GetNextResult::ReturnStatus::kEOF;
            return nextInput;
        } else {
            // Ensure that the client's operationTime reflects the latest write even if the command
            // fails.
            ON_BLOCK_EXIT([&] {
                pExpCtx->getMongoProcessInterface()->updateClientOperationTime(
                    pExpCtx->getOperationContext());
            });

            if (!_initialized) {
                initialize();
                _initialized = true;
            }

            // While most metadata attached to a command is limited to less than a KB, Audit
            // metadata may grow to an arbitrary size.
            //
            // Ask the active Client how much Audit metadata we'll use for it, add in
            // our own estimate of write header size, and assume that the rest can fit in the space
            // reserved by BSONObjMaxUserSize's overhead plus the value from the server parameter:
            // internalQueryDocumentSourceWriterBatchExtraReservedBytes.
            const auto estimatedMetadataSizeBytes =
                rpc::estimateAuditMetadataSize(pExpCtx->getOperationContext());

            BatchedCommandRequest batchWrite = makeBatchedWriteRequest();
            const auto writeHeaderSize = estimateWriteHeaderSize(batchWrite);
            const auto initialRequestSize = estimatedMetadataSizeBytes + writeHeaderSize +
                internalQueryDocumentSourceWriterBatchExtraReservedBytes.load();

            uassert(
                7637800,
                fmt::format("Unable to proceed with write while metadata size ({}KB) exceeds {}KB",
                            initialRequestSize / 1024,
                            BSONObjMaxUserSize / 1024),
                initialRequestSize <= BSONObjMaxUserSize);

            const auto maxBatchSizeBytes = BSONObjMaxUserSize - initialRequestSize;

            BatchedObjects batch;
            size_t bufferedBytes = 0;
            auto nextInput = pSource->getNext();
            for (; nextInput.isAdvanced(); nextInput = pSource->getNext()) {
                waitWhileFailPointEnabled();

                auto doc = nextInput.releaseDocument();
                auto [obj, objSize] = makeBatchObject(std::move(doc));

                bufferedBytes += objSize;
                if (!batch.empty() &&
                    (bufferedBytes > maxBatchSizeBytes ||
                     batch.size() >= write_ops::kMaxWriteBatchSize)) {
                    flush(std::move(batchWrite), std::move(batch));
                    batch.clear();
                    batchWrite = makeBatchedWriteRequest();
                    bufferedBytes = objSize;
                }
                batch.push_back(std::move(obj));
            }
            if (!batch.empty()) {
                flush(std::move(batchWrite), std::move(batch));
                batch.clear();
            }

            switch (nextInput.getStatus()) {
                case GetNextResult::ReturnStatus::kAdvanced: {
                    MONGO_UNREACHABLE;  // We consumed all advances above.
                }
                case GetNextResult::ReturnStatus::kAdvancedControlDocument: {
                    MONGO_UNREACHABLE_TASSERT(
                        10358904);  // No support for control events in this document source.
                }
                case GetNextResult::ReturnStatus::kPauseExecution: {
                    return nextInput;  // Propagate the pause.
                }
                case GetNextResult::ReturnStatus::kEOF: {
                    _done = true;
                    finalize();
                    return nextInput;
                }
            }
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Prepares the stage to be able to write incoming batches.
     */
    virtual void initialize() {}

    /**
     * Finalize the output collection, called when there are no more documents to write.
     */
    virtual void finalize() {}

    /**
     * Writes the documents in 'batch' to the output namespace via 'bcr'.
     */
    virtual void flush(BatchedCommandRequest bcr, BatchedObjects batch) = 0;

    /**
     * Estimates the size of the header of a batch write (that is, the size of the write command
     * minus the size of write statements themselves).
     */
    int estimateWriteHeaderSize(const BatchedCommandRequest& bcr) const {
        using BatchType = BatchedCommandRequest::BatchType;
        switch (bcr.getBatchType()) {
            case BatchType::BatchType_Insert:
                return _writeSizeEstimator->estimateInsertHeaderSize(bcr.getInsertRequest());
            case BatchType::BatchType_Update:
                return _writeSizeEstimator->estimateUpdateHeaderSize(bcr.getUpdateRequest());
            case BatchType::BatchType_Delete:
                break;
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Constructs and configures a BatchedCommandRequest for performing a batch write.
     */
    virtual BatchedCommandRequest makeBatchedWriteRequest() const = 0;

    /**
     * Creates a batch object from the given document and returns it to the caller along with the
     * object size.
     */
    virtual std::pair<B, int> makeBatchObject(Document doc) const = 0;

    /**
     * A subclass may override this method to enable a fail point right after a next input element
     * has been retrieved, but not processed yet.
     */
    virtual void waitWhileFailPointEnabled() {}

    // An interface that is used to estimate the size of each write operation.
    const std::unique_ptr<MongoProcessInterface::WriteSizeEstimator> _writeSizeEstimator;

protected:
    const NamespaceString _outputNs;

    bool _initialized{false};
    bool _done{false};
};

}  // namespace mongo::exec::agg
