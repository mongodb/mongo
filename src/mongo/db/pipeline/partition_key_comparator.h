/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
