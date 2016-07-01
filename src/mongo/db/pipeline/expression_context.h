/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <memory>
#include <string>

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_comparator.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

struct ExpressionContext : public IntrusiveCounterUnsigned {
public:
    ExpressionContext() = default;

    ExpressionContext(OperationContext* opCtx, const AggregationRequest& request);

    /**
     * Used by a pipeline to check for interrupts so that killOp() works. Throws a UserAssertion if
     * this aggregation pipeline has been interrupted.
     */
    void checkForInterrupt();

    void setCollator(std::unique_ptr<CollatorInterface> coll);

    const CollatorInterface* getCollator() const {
        return _collator.get();
    }

    const DocumentComparator& getDocumentComparator() const {
        return _documentComparator;
    }

    const ValueComparator& getValueComparator() const {
        return _valueComparator;
    }

    bool isExplain = false;
    bool inShard = false;
    bool inRouter = false;
    bool extSortAllowed = false;
    bool bypassDocumentValidation = false;

    NamespaceString ns;
    std::string tempDir;  // Defaults to empty to prevent external sorting in mongos.

    OperationContext* opCtx;

    // Collation requested by the user for this pipeline. Empty if the user did not request a
    // collation.
    const BSONObj collation;

    static const int kInterruptCheckPeriod = 128;
    int interruptCounter = kInterruptCheckPeriod;  // when 0, check interruptStatus

private:
    // Collator used to compare elements. 'collator' is initialized from 'collation', except in the
    // case where 'collation' is empty and there is a collection default collation.
    std::unique_ptr<CollatorInterface> _collator;

    // Used for all comparisons of Document/Value during execution of the aggregation operation.
    DocumentComparator _documentComparator;
    ValueComparator _valueComparator;
};

}  // namespace mongo
