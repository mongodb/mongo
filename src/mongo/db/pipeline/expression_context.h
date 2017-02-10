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
#include <boost/optional.hpp>
#include <memory>
#include <string>
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregation_request.h"
#include "mongo/db/pipeline/document_comparator.h"
#include "mongo/db/pipeline/value_comparator.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

namespace mongo {

class ExpressionContext : public RefCountable {
public:
    struct ResolvedNamespace {
        ResolvedNamespace() = default;
        ResolvedNamespace(NamespaceString ns, std::vector<BSONObj> pipeline);

        NamespaceString ns;
        std::vector<BSONObj> pipeline;
    };

    /**
     * Constructs an ExpressionContext to be used for Pipeline parsing and evaluation.
     * 'resolvedNamespaces' maps collection names (not full namespaces) to ResolvedNamespaces.
     */
    ExpressionContext(OperationContext* opCtx,
                      const AggregationRequest& request,
                      std::unique_ptr<CollatorInterface> collator,
                      StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces);

    /**
     * Used by a pipeline to check for interrupts so that killOp() works. Throws a UserAssertion if
     * this aggregation pipeline has been interrupted.
     */
    void checkForInterrupt();

    const CollatorInterface* getCollator() const {
        return _collator.get();
    }

    const DocumentComparator& getDocumentComparator() const {
        return _documentComparator;
    }

    const ValueComparator& getValueComparator() const {
        return _valueComparator;
    }

    /**
     * Returns an ExpressionContext that is identical to 'this' that can be used to execute a
     * separate aggregation pipeline on 'ns'.
     */
    boost::intrusive_ptr<ExpressionContext> copyWith(NamespaceString ns) const;

    /**
     * Returns the ResolvedNamespace corresponding to 'nss'. It is an error to call this method on a
     * namespace not involved in the pipeline.
     */
    const ResolvedNamespace& getResolvedNamespace(const NamespaceString& nss) const {
        auto it = _resolvedNamespaces.find(nss.coll());
        invariant(it != _resolvedNamespaces.end());
        return it->second;
    };

    // The explain verbosity requested by the user, or boost::none if no explain was requested.
    boost::optional<ExplainOptions::Verbosity> explain;

    bool inShard = false;
    bool inRouter = false;
    bool extSortAllowed = false;
    bool bypassDocumentValidation = false;

    NamespaceString ns;
    std::string tempDir;  // Defaults to empty to prevent external sorting in mongos.

    OperationContext* opCtx;

    // Collation requested by the user for this pipeline. Empty if the user did not request a
    // collation.
    BSONObj collation;

protected:
    static const int kInterruptCheckPeriod = 128;

    /**
     * Should only be used by 'ExpressionContextForTest'.
     */
    ExpressionContext() = default;

    /**
     * Sets '_collator' and resets '_documentComparator' and '_valueComparator'.
     *
     * Use with caution - it is illegal to change the collation once a Pipeline has been parsed with
     * this ExpressionContext.
     */
    void setCollator(std::unique_ptr<CollatorInterface> collator);

    // Collator used for comparisons.
    std::unique_ptr<CollatorInterface> _collator;

    // Used for all comparisons of Document/Value during execution of the aggregation operation.
    // Must not be changed after parsing a Pipeline with this ExpressionContext.
    DocumentComparator _documentComparator;
    ValueComparator _valueComparator;

    // A map from namespace to the resolved namespace, in case any views are involved.
    StringMap<ResolvedNamespace> _resolvedNamespaces;

    int _interruptCounter = kInterruptCheckPeriod;
};

}  // namespace mongo
