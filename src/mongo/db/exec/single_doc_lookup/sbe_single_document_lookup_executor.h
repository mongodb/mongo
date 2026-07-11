// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/ordering.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/single_doc_lookup/collection_acquirer.h"
#include "mongo/db/exec/single_doc_lookup/local_lookup_eligibility.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_stats.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/uuid.h"

namespace mongo::exec::agg {

/**
 * SBE-based single-document lookup strategy.
 *
 * Builds a parameterized SBE plan on first use and reuses it across lookups by rebinding the
 * document key value directly into the plan's runtime-env slots. The collection acquisition is
 * taken via the injected CollectionAcquirer and cached across lookups. This amortizes the
 * per-event acquire cost the same way the SBE plan state is amortized.
 */
class SbeSingleDocumentLookupExecutor : public SingleDocumentLookupExecutor {
public:
    SbeSingleDocumentLookupExecutor(
        std::unique_ptr<CollectionAcquirer> collectionAcquirer,
        std::unique_ptr<LocalLookupEligibility> localEligibility,
        boost::optional<SingleDocumentLookupStatsRecorder> recorder = boost::none)
        : _collectionAcquirer(std::move(collectionAcquirer)),
          _localEligibility(std::move(localEligibility)),
          _recorder(std::move(recorder)) {
        tassert(12952800,
                "SbeSingleDocumentLookupExecutor requires a non-null collection acquirer",
                _collectionAcquirer);
        tassert(12952801,
                "SbeSingleDocumentLookupExecutor requires a non-null eligibility policy",
                _localEligibility);
    }

    SbeSingleDocumentLookupExecutor(const SbeSingleDocumentLookupExecutor&) = delete;
    SbeSingleDocumentLookupExecutor& operator=(const SbeSingleDocumentLookupExecutor&) = delete;
    SbeSingleDocumentLookupExecutor(SbeSingleDocumentLookupExecutor&&) = delete;
    SbeSingleDocumentLookupExecutor& operator=(SbeSingleDocumentLookupExecutor&&) = delete;

    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override;

    /**
     * Drops all cached state: the plan and the acquisition. Rebuilt from scratch on the next
     * performLookup. Idempotent and non-throwing.
     */
    void releaseResources() noexcept override;

    /**
     * Opt-in: when set, totalKeys/DocsExamined accumulated by the cached SBE plan are folded into
     * 'sink' whenever the plan is torn down (releaseResources()/resetPlan()) rather than after
     * every lookup.
     */
    void setPlanSummaryStatsSink(PlanSummaryStats* sink) override {
        _planSummaryStatsSink = sink;
    }

    /**
     * Test-only: whether the strategy currently holds attached catalog state (an active plan with
     * open cursors and/or a cached acquisition handle). False after releaseResources().
     */
    bool holdsAttachedCatalogState_forTest() const {
        return (_preparedExecutor.has_value() &&
                _preparedExecutor->planState == PreparedExecutor::PlanState::kActive) ||
            _collectionAcquisition.has_value();
    }

    /**
     * Test-only: returns the cached SBE plan root after the first successful performLookup. Null
     * before the first call or if planning fell back. Used by unit tests to assert plan shape
     * (e.g. "the lookup must use an index scan, not a record-store scan").
     */
    const sbe::PlanStage* getCachedPlanRoot_forTest() const {
        return _preparedExecutor ? _preparedExecutor->root.get() : nullptr;
    }

    /**
     * Test-only: number of times PreparedExecutor::make() has produced a plan for this strategy.
     * Pointer identity of getCachedPlanRoot_forTest() is not a reliable rebuild signal (a freed
     * plan's heap address can be reused by the very next allocation); this counter is.
     */
    size_t getPlanRebuildCount_forTest() const {
        return _planRebuildCount_forTest;
    }

private:
    /**
     * Binder that, given a documentKey _id BSONElement, writes the slot value(s) for the single
     * parameterized _id leaf in the plan. Created once at executor creation; rebinds on every
     * performLookup() call.
     *
     * Supported plan shapes:
     *   kIxscanKeyPair        : _id_ IXSCAN with a SingleIntervalPlan slot pair (non-clustered).
     *   kClusteredRecordIdPair: bounded STAGE_COLLSCAN with min/max RecordId slots (clustered).
     */
    struct SlotBinder {
        enum class Kind : uint8_t {
            /** Single-field IXSCAN equality. Value encoded as KeyString; written to slotA (low) /
             * slotB (high). Both bounds inclusive [v, v]. */
            kIxscanKeyPair,

            /** Clustered-scan bounds. Value converted to RecordId via record_id_helpers::keyForElem
             * (handles scalar and compound BSON _id). Min == max for a point seek. */
            kClusteredRecordIdPair,
        };

        /**
         * Inspects plan-stage metadata and returns a SlotBinder for the supported _id plan shapes,
         * or boost::none for shapes that can't be expressed as a single slot-pair rebind.
         */
        static boost::optional<SlotBinder> make(const stage_builder::PlanStageData& data);

        /**
         * Encodes 'idElem' into the slot value type for this binder and writes it into the runtime
         * env. Returns false for unsupported _id types so the caller can decline with kNotHandled.
         */
        bool bind(const BSONElement& idElem, sbe::RuntimeEnvironment* env) const;

        Kind kind;
        sbe::value::SlotId slotA;  // lowKey / minRecord
        sbe::value::SlotId slotB;  // highKey / maxRecord

        // kIxscanKeyPair-only:
        key_string::Version ksVersion;
        Ordering ordering = Ordering::allAscending();
        int direction = 1;

        // Borrowed from the _id index's catalog entry or null for a simple index. Safe to borrow:
        // the executor (and this binder) is always torn down via resetPlan() before the acquisition
        // is dropped, so it never outlives the catalog entry it points into.
        const CollatorInterface* collator = nullptr;
    };

    /**
     * Compiled SBE executor kept alive across performLookup() calls within one batch window.
     */
    struct PreparedExecutor {
        /**
         * The cached plan's lifecycle.
         */
        enum class PlanState : uint8_t {
            // Freshly built, SBE-saved. reOpen must be false.
            kUnopened,
            // Opened at least once, then saved back by releaseResources(). reOpen must be true.
            kSaved,
            // Restored and open; holding storage cursors.
            kActive,
        };

        /**
         * Plans and compiles a fresh SBE executor for the given collection + canonical query.
         * Returns boost::none when planning produces no solutions or the plan shape is not
         * supported by SlotBinder.
         */
        static boost::optional<PreparedExecutor> make(OperationContext* opCtx,
                                                      const CollectionAcquirer::Handle& coll,
                                                      std::unique_ptr<CanonicalQuery> cq);

        std::unique_ptr<sbe::PlanStage> root;
        stage_builder::PlanStageData data;

        /**
         * Owned here because the PlanStage tree holds a raw pointer to the policy; it must outlive
         * the plan. INTERRUPT_ONLY: no self-yielding; acquisition lifecycle is owned by the window.
         */
        std::unique_ptr<PlanYieldPolicySBE> yieldPolicy;

        /** Built once and reused across every lookup for this compilation of the plan. */
        std::unique_ptr<CanonicalQuery> cq;

        /** Collection identity at plan-build time; used to detect DDL invalidation. */
        UUID collectionUUID;
        size_t collectionVersion;

        /** Slot binder for this executor. */
        SlotBinder slotBinder;

        /** State of the current executor. */
        PlanState planState = PlanState::kUnopened;

        /**
         * Whether this plan is currently attached to an OperationContext. make() leaves it attached
         * (prepareSlotBasedExecutableTree() attaches as a side effect).
         */
        bool attachedToOpCtx = true;

        /**
         * Baseline writePlanSummaryStats() diffs against to fold in only the delta. Scoped to
         * the SingleDocumentLookupExecutor, not the SBE executor: the plan can be recreated (DDL
         * invalidation, exceptions), and a new one's counters restart at 0, so the baseline must
         * too.
         */
        size_t lastAccumulatedKeysExamined = 0;
        size_t lastAccumulatedDocsExamined = 0;
    };

    /**
     * Returns the cached CollectionAcquirer::Handle, acquiring it fresh on cache miss. Throws
     * CollectionUUIDMismatch when a cached acquisition's UUID no longer matches the requested one
     * (DDL between calls in the window); the caller's catch reports the document as not found and
     * drops the cache.
     */
    CollectionAcquirer::Handle& getOrAcquireCollection(OperationContext* opCtx,
                                                       const NamespaceString& nss,
                                                       boost::optional<UUID> uuid);

    /**
     * Builds the SBE plan on first call (or after an invalidation); reuses it on every subsequent
     * call. Returns false if planning failed and the caller should fall back (kNotHandled); true
     * otherwise.
     */
    bool getOrMakeExecutor(OperationContext* opCtx,
                           const CollectionAcquirer::Handle& coll,
                           const NamespaceString& nss,
                           const BSONObj& filter);

    /**
     * open(reOpen) + getNext on the cached plan. Transitions the plan to active on the first call
     * after construction or after a releaseResources(), then leaves it active for the remainder of
     * the current batch window; releaseResources() saves it back.
     */
    LookupResult executeOnce(OperationContext* opCtx);

    /**
     * A PlanSummaryStats describing which index this lookup's plan used, for $indexStats. SBE
     * cannot derive this from the runtime stats tree (PlanSummaryStatsVisitor only fills keys/docs
     * examined), so it is determined statically from the plan shape: an _id ixscan reports the
     * _id_ index; a clustered bounded _id scan reports nothing, matching what Express records for
     * the same clustered lookup. Only the index-usage fields are set here; keys/docs examined are
     * folded into the sink separately by writePlanSummaryStats().
     */
    PlanSummaryStats indexUsageSummary() const;

    /**
     * Folds the delta between the cached plan's current cumulative totalKeys/DocsExamined and the
     * last-observed values into '_planSummaryStatsSink'.
     */
    void writePlanSummaryStats() noexcept;

    /** Drops the cached plan (but not the acquisition). */
    void resetPlan() noexcept;

    /**
     * Drops all cached state: the plan and the acquisition. noexcept so it can run inside the
     * performLookup ScopeGuard during exception unwinding.
     */
    void resetCachedState() noexcept;

    /**
     * Drops the cached acquisition and its derived '_acquisitionState' together, in that order:
     * '_acquisitionState' can hold a reference into the acquisition's ownership filter, so it must
     * be cleared before the acquisition it points into is destroyed.
     */
    void resetAcquisition() noexcept;

    std::unique_ptr<CollectionAcquirer> _collectionAcquirer;
    std::unique_ptr<LocalLookupEligibility> _localEligibility;
    boost::optional<SingleDocumentLookupStatsRecorder> _recorder;

    /**
     * Cached executor and its state across calls. See PreparedExecutor::PlanState.
     */
    boost::optional<PreparedExecutor> _preparedExecutor;

    /**
     * Cached CollectionAcquirer::Handle. Populated on first performLookup() and reused for
     * subsequent lookups. Dropped by releaseResources() and on any error path where the catalog
     * state may have changed.
     */
    boost::optional<CollectionAcquirer::Handle> _collectionAcquisition;

    /**
     * Mirrors what '_collectionAcquisition' currently holds, in the shape LocalLookupEligibility
     * needs.
     */
    LocalLookupEligibility::AcquisitionState _acquisitionState =
        LocalLookupEligibility::NoHeldAcquisition{};

    /**
     * Non-owning. Owned by the caller.
     */
    PlanSummaryStats* _planSummaryStatsSink = nullptr;

    /**
     * Test-only. See getPlanRebuildCount_forTest().
     */
    size_t _planRebuildCount_forTest = 0;
};

}  // namespace mongo::exec::agg
