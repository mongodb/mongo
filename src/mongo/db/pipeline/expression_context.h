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
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/tailable_mode.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

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
     * An RAII type that will temporarily change the ExpressionContext's collator. Resets the
     * collator to the previous value upon destruction.
     */
    class CollatorStash {
    public:
        /**
         * Resets the collator on '_expCtx' to the original collator present at the time this
         * CollatorStash was constructed.
         */
        ~CollatorStash();

    private:
        /**
         * Temporarily changes the collator on 'expCtx' to be 'newCollator'. The collator will be
         * set back to the original value when this CollatorStash is deleted.
         *
         * This constructor is private, all CollatorStashes should be created by calling
         * ExpressionContext::temporarilyChangeCollator().
         */
        CollatorStash(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                      std::unique_ptr<CollatorInterface> newCollator);

        friend class ExpressionContext;

        boost::intrusive_ptr<ExpressionContext> _expCtx;

        BSONObj _originalCollation;
        std::unique_ptr<CollatorInterface> _originalCollatorOwned;
        const CollatorInterface* _originalCollatorUnowned{nullptr};
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
     * Constructs an ExpressionContext to be used for MatchExpression parsing outside of the context
     * of aggregation.
     */
    ExpressionContext(OperationContext* opCtx, const CollatorInterface* collator);

    /**
     * Used by a pipeline to check for interrupts so that killOp() works. Throws a UserAssertion if
     * this aggregation pipeline has been interrupted.
     */
    void checkForInterrupt();

    const CollatorInterface* getCollator() const {
        return _collator;
    }

    void setCollator(const CollatorInterface* collator);

    const DocumentComparator& getDocumentComparator() const {
        return _documentComparator;
    }

    const ValueComparator& getValueComparator() const {
        return _valueComparator;
    }

    /**
     * Temporarily resets the collator to be 'newCollator'. Returns a CollatorStash which will reset
     * the collator back to the old value upon destruction.
     */
    std::unique_ptr<CollatorStash> temporarilyChangeCollator(
        std::unique_ptr<CollatorInterface> newCollator);

    /**
     * Returns an ExpressionContext that is identical to 'this' that can be used to execute a
     * separate aggregation pipeline on 'ns' with the optional 'uuid'.
     */
    boost::intrusive_ptr<ExpressionContext> copyWith(
        NamespaceString ns,
        boost::optional<UUID> uuid = boost::none,
        boost::optional<std::unique_ptr<CollatorInterface>> collator = boost::none) const;

    /**
     * Returns the ResolvedNamespace corresponding to 'nss'. It is an error to call this method on a
     * namespace not involved in the pipeline.
     */
    const ResolvedNamespace& getResolvedNamespace(const NamespaceString& nss) const {
        auto it = _resolvedNamespaces.find(nss.coll());
        invariant(it != _resolvedNamespaces.end());
        return it->second;
    };

    /**
     * Convenience call that returns true if the tailableMode indicates a tailable and awaitData
     * query.
     */
    bool isTailableAwaitData() const {
        return tailableMode == TailableMode::kTailableAndAwaitData;
    }

    // The explain verbosity requested by the user, or boost::none if no explain was requested.
    boost::optional<ExplainOptions::Verbosity> explain;

    // The comment provided by the user, or the empty string if no comment was provided.
    std::string comment;

    bool fromMongos = false;
    bool needsMerge = false;
    bool inMongos = false;
    bool allowDiskUse = false;
    bool bypassDocumentValidation = false;

    // We track whether the aggregation request came from a 3.4 mongos. If so, the merge may occur
    // on a 3.4 shard (which does not understand sort key metadata), and we should not serialize the
    // sort key.
    // TODO SERVER-30924: remove this.
    bool from34Mongos = false;

    NamespaceString ns;
    boost::optional<UUID> uuid;
    std::string tempDir;  // Defaults to empty to prevent external sorting in mongos.

    OperationContext* opCtx;

    const TimeZoneDatabase* timeZoneDatabase;

    // Collation requested by the user for this pipeline. Empty if the user did not request a
    // collation.
    BSONObj collation;

    Variables variables;
    VariablesParseState variablesParseState;

    TailableMode tailableMode = TailableMode::kNormal;

    // Tracks the depth of nested aggregation sub-pipelines. Used to enforce depth limits.
    size_t subPipelineDepth = 0;

protected:
    static const int kInterruptCheckPeriod = 128;

    ExpressionContext(NamespaceString nss, const TimeZoneDatabase* tzDb)
        : ns(std::move(nss)),
          timeZoneDatabase(tzDb),
          variablesParseState(variables.useIdGenerator()) {}

    /**
     * Sets '_ownedCollator' and resets '_collator', 'documentComparator' and 'valueComparator'.
     *
     * Use with caution - '_ownedCollator' is used in the context of a Pipeline, and it is illegal
     * to change the collation once a Pipeline has been parsed with this ExpressionContext.
     */
    void setCollator(std::unique_ptr<CollatorInterface> collator) {
        _ownedCollator = std::move(collator);
        setCollator(_ownedCollator.get());
    }

    friend class CollatorStash;

    // Collator used for comparisons. This is owned in the context of a Pipeline.
    // TODO SERVER-31294: Move ownership of an aggregation's collator elsewhere.
    std::unique_ptr<CollatorInterface> _ownedCollator;

    // Collator used for comparisons. If '_ownedCollator' is non-null, then this must point to the
    // same collator object.
    const CollatorInterface* _collator = nullptr;

    // Used for all comparisons of Document/Value during execution of the aggregation operation.
    // Must not be changed after parsing a Pipeline with this ExpressionContext.
    DocumentComparator _documentComparator;
    ValueComparator _valueComparator;

    // A map from namespace to the resolved namespace, in case any views are involved.
    StringMap<ResolvedNamespace> _resolvedNamespaces;

    int _interruptCounter = kInterruptCheckPeriod;
};

}  // namespace mongo
