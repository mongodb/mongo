// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Performs comparisons across documents in an aggregation pipeline to determine when a new
 * partition starts.
 */
class PartitionKeyComparator {
public:
    PartitionKeyComparator(ExpressionContext* expCtx,
                           boost::intrusive_ptr<Expression> partitionExpr,
                           Document firstDoc)
        : _expCtx(expCtx), _expr(std::move(partitionExpr)) {
        tassert(5733800, "Null expression context passed to PartitionKeyComparator", _expCtx);
        tassert(5733801, "Null expression passed to PartitionKeyComparator", _expr);
        _partitionValue = evaluateAndCoerce(firstDoc);
    }

    /**
     * Check to see if the passed in 'doc' begins a new partition.
     */
    bool isDocumentNewPartition(Document doc) {
        auto newVal = evaluateAndCoerce(doc);
        // If one of the values are different, we are in a new partition.
        if (_expCtx->getValueComparator().compare(newVal, _partitionValue) != 0) {
            _partitionValue = std::move(newVal);
            return true;
        }

        return false;
    }

    auto getApproximateSize() const {
        return _partitionValue.getApproximateSize();
    }

private:
    Value evaluateAndCoerce(Document doc) {
        // We assume that partitioning is achieved by sorting, and missing fields and nulls are
        // considered equivalent in sorting. Documents with missing fields and nulls may
        // interleave with each other, resulting in these documents processed into many separate
        // partitions (null, missing, null, missing). However, it is still guranteed that all nulls
        // and missing values will be grouped together after sorting. To address this issue, we
        // coerce documents with the missing fields to null partition, which is also consistent with
        // the approach in $group.
        auto retValue = _expr->evaluate(doc, &_expCtx->variables);
        uassert(ErrorCodes::TypeMismatch,
                "An expression used to partition cannot evaluate to value of type array",
                !retValue.isArray());
        return retValue.missing() ? Value(BSONNULL) : std::move(retValue);
    }
    ExpressionContext* _expCtx;
    boost::intrusive_ptr<Expression> _expr;
    Value _partitionValue;
};
}  // namespace mongo
