// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/memory_tracking/memory_usage_limit.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <boost/noncopyable.hpp>

namespace mongo {
/**
 * Memory usage tracker for use cases where we don't need per-function memory tracking.
 *
 * TODO SERVER-113197: Remove streams dependency on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] SimpleMemoryUsageTracker {
public:
    SimpleMemoryUsageTracker(const SimpleMemoryUsageTracker&) = delete;
    SimpleMemoryUsageTracker operator=(const SimpleMemoryUsageTracker&) = delete;

    SimpleMemoryUsageTracker(SimpleMemoryUsageTracker&&) = default;
    SimpleMemoryUsageTracker& operator=(SimpleMemoryUsageTracker&&) = default;

    SimpleMemoryUsageTracker(SimpleMemoryUsageTracker* base,
                             MemoryUsageLimit maxAllowedMemoryUsageBytes);

    explicit SimpleMemoryUsageTracker(MemoryUsageLimit maxAllowedMemoryUsageBytes =
                                          MemoryUsageLimit{std::numeric_limits<int64_t>::max()});

    /**
     * Accumulates 'fullDiff' into this tracker's full counter and 'uniqueDiff' into its unique
     * counter, and forwards both up the base chain.
     *
     * The zero case is handled inline because it is hot and does nothing. Fixed-size accumulators
     * make it common: $group calls add() once per input document with
     * 'accumulator->getMemUsage() - prevMemUsage', which is always 0 for $sum and $count.
     */
    MONGO_COMPILER_ALWAYS_INLINE void add(int64_t fullDiff, int64_t uniqueDiff) {
        if (fullDiff == 0 && uniqueDiff == 0) {
            return;
        }
        addInternal(fullDiff, uniqueDiff);
    }

    /**
     * Single-figure forms charge the same bytes to both figures. Defined inline so that callers
     * reach the zero-diff early return in 'add()' without paying for an out-of-line call.
     */
    MONGO_COMPILER_ALWAYS_INLINE void add(int64_t diff) {
        add(diff, diff);
    }

    MONGO_COMPILER_ALWAYS_INLINE void set(int64_t total) {
        set(total, total);
    }

    MONGO_COMPILER_ALWAYS_INLINE void set(int64_t fullTotal, int64_t uniqueTotal) {
        add(fullTotal - _inUseFullMemoryBytes, uniqueTotal - _inUseUniqueMemoryBytes);
    }

    /** The figure the limit is checked against: unique for the operation tracker, else full. */
    int64_t inUseTrackedMemoryBytes() const {
        return limitFigure();
    }

    /**
     * The unique counter, on every tracker. For the operation tracker it is the total bytes that
     * have been charged. For other trackers it is the number of bytes not charged to the operation
     * tracker at allocation time.
     */
    int64_t inUseUniqueMemoryBytes() const {
        return _inUseUniqueMemoryBytes;
    }

    /**
     * The highest observed inUseTrackedMemoryBytes(): for the operation tracker the maximum unique
     * memory bytes, otherwise the maximum full memory bytes.
     */
    int64_t peakTrackedMemoryBytes() const {
        return _peakTrackedMemoryBytes;
    }

    /**
     * Returns true only if this tracker and every ancestor in the base chain are within their
     * respective limits. A stage is compared on its full figure, the operation tracker on its
     * unique one; see 'limitFigure()'. This is the check whose failure fails the operation.
     */
    bool withinMemoryLimit(OperationContext* opCtx) const {
        return limitFigure() <= _maxAllowedMemoryUsageBytes.get(opCtx) &&
            (!_base || _base->withinMemoryLimit(opCtx));
    }

    /**
     * Returns how many more bytes can be added before this tracker or any ancestor in the base
     * chain would exceed its own limit -- i.e. the same chain 'withinMemoryLimit()' checks, but
     * expressed as a byte budget (the minimum headroom across the chain) rather than a boolean.
     * Can be negative if some ancestor is already over its limit. Useful for sizing a heuristic
     * (e.g. how much a caller may batch before it must check in with the tracker) without that
     * heuristic having to know how deep or where in the chain the binding limit actually is.
     */
    int64_t remainingMemoryUsageBytes(OperationContext* opCtx) const {
        int64_t remaining = _maxAllowedMemoryUsageBytes.get(opCtx) - limitFigure();
        if (_base) {
            remaining = std::min(remaining, _base->remainingMemoryUsageBytes(opCtx));
        }
        return remaining;
    }

    int64_t maxAllowedMemoryUsageBytes(OperationContext* opCtx) const {
        return _maxAllowedMemoryUsageBytes.get(opCtx);
    }

    /**
     * Prefer this over 'maxAllowedMemoryUsageBytes()' when handing the limit to another tracker:
     * copying the wrapper preserves how the limit is resolved, not just its current value.
     */
    const MemoryUsageLimit& maxAllowedMemoryUsageLimit() const {
        return _maxAllowedMemoryUsageBytes;
    }

    /**
     * Throws ExceededMemoryLimit if the current usage exceeds the limit, including a
     * name, stageName (optional), current usage, and limit in the error message.
     */
    void assertWithinMemoryLimit(OperationContext* opCtx,
                                 std::string_view name,
                                 std::string_view stageName = {}) const {
        if (MONGO_likely(withinMemoryLimit(opCtx))) {
            return;
        }
        uassertedMemoryLimitExceeded(opCtx, name, stageName);
    }

    /**
     * Checks that the caller can spill to disk if necessary.
     * Throws QueryExceededMemoryLimitNoDiskUseAllowed if spilling to disk is not allowed.
     */
    void assertCanSpill(bool canSpill, std::string_view name = {}) const;

    /**
     * Returns a new SimpleMemoryUsageTracker. The copy constructor for this class is purposefully
     * deleted - use this method instead. Note that the members _peakTrackedMemoryBytes,
     * _inUseFullMemoryBytes and _inUseUniqueMemoryBytes will be initialized to zero.
     */
    SimpleMemoryUsageTracker makeFreshSimpleMemoryUsageTracker() const;

    /**
     * Re-point this tracker's parent ('base') to 'base', moving this tracker's current in-use bytes
     * off the old base and onto the new one so the ancestor chain's totals (and CurOp stats) stay
     * consistent across the rebind. Pass nullptr to detach from the base entirely. Intended for
     * stages whose lifetime spans getMore opCtx swaps: they detach from the (about-to-be-freed)
     * operation tracker and later re-bind to the current operation's tracker on reattach. Callers
     * must invoke this while the current base is still alive.
     *
     * TODO SERVER-131203: this is a stopgap and is NOT for general use -- it exists specifically to
     * let BatchedEnrichmentStage rebind its tracker base across getMore opCtx swaps. Remove it once
     * that stage's memory tracking is properly integrated with the operation memory tracker.
     */
    void resetBase(SimpleMemoryUsageTracker* base);

    friend class MemoryUsageTracker;

protected:
    /**
     * Latches the chunk size gating CurOp writes ('0' means "no chunking" and is clamped to 1).
     * Only meaningful on a tracker that owns a '_writeToCurOp' callback; the public constructors'
     * default of 1 (report every change) is inert otherwise.
     */
    SimpleMemoryUsageTracker(SimpleMemoryUsageTracker* base,
                             MemoryUsageLimit maxAllowedMemoryUsageBytes,
                             int64_t curOpWriteChunkSize);

    /**
     * Marks this tracker as the operation tracker. Explicit rather than inferred from a missing
     * '_base' or '_writeToCurOp', since a tracker can lose either without changing role.
     */
    void setIsOperationTracker() {
        _isOperationTracker = true;
    }

    /**
     * Provide an extra function that is called whenever add() is invoked. Let it be set via this
     * method instead in the constructor to allow subclasses to capture "this."
     */
    void setWriteToCurOp(std::function<void(int64_t, int64_t)> writeToCurOp);

    /**
     * Publishes the current exact totals through '_writeToCurOp' for publishes made outside of
     * addInternal()'s chunk-crossing path -- e.g. when a stashed tracker is reattached to a resumed
     * getMore's opCtx. Consumes the "never reported" state, so the return-to-zero rule still drives
     * CurOp back to zero after a release that follows the publish.
     */
    void flushToCurOp() {
        if (!_writeToCurOp) {
            return;
        }

        reportToCurOp();
        const int64_t figure = limitFigure();
        _lastReportedLowerBound =
            figure == 0 ? -1 : (figure / _curOpWriteChunkSize) * _curOpWriteChunkSize;
    }

    /**
     * Invokes '_writeToCurOp' with the current totals. Kept out of line so that addInternal() has
     * no address-taken locals: std::function's invoker takes its arguments by reference, which
     * would otherwise put the totals on the stack and, because '-fstack-protector-strong' is
     * enabled globally, add a canary prologue/epilogue to every update including those that do not
     * report. It does not save the stack frame itself -- '-fno-omit-frame-pointer' is also global.
     *
     * The canary is then suppressed here as well, which is what keeps the out-of-lining from simply
     * moving the cost onto the reporting path: with it, an update that does report was measured
     * ~11% slower than not out-of-lining at all, and without it that path is at parity while the
     * non-reporting paths keep the full benefit. Safe to suppress because this function has no
     * buffer on its stack -- it passes two int64_t by reference to the callback and nothing else.
     */
    MONGO_COMPILER_NO_STACK_PROTECTOR void reportToCurOp() const;

private:
    /**
     * Adds both diffs to this tracker's own counters and forwards them to the base. Every tracker
     * does the same, the operation tracker included; only 'limitFigure()' tells the roles apart.
     */
    MONGO_COMPILER_NOINLINE void addInternal(int64_t fullDiff, int64_t uniqueDiff);


    /**
     * Called after the memory limit has already been checked to assert that the current usage
     * exceeds the limit, including a name, stageName (optional), current usage, and limit in
     * the error message.
     */
    MONGO_COMPILER_NORETURN void uassertedMemoryLimitExceeded(OperationContext* opCtx,
                                                              std::string_view name,
                                                              std::string_view stageName) const;

    SimpleMemoryUsageTracker* _base = nullptr;

    // Maximum memory consumption thus far observed for this function.
    int64_t _peakTrackedMemoryBytes = 0;
    // Everything this tracker's scope holds, including bytes already charged at their allocation.
    int64_t _inUseFullMemoryBytes = 0;
    // The part of the above not already charged at its allocation. Underflow means a charge and its
    // release resolved to different trackers.
    int64_t _inUseUniqueMemoryBytes = 0;
    // True only for the operation tracker, whose limit uses the unique figure so that no byte is
    // counted twice.
    bool _isOperationTracker = false;

    // This tracker's '_writeToCurOp' is only invoked when usage surpasses this size. Writing to
    // CurOp involves lock contention, so in performance-sensitive situations, we should set a
    // non-1 size. Constructors clamp the requested chunk size to at least 1: 1 means "report every
    // change." Only meaningful on a tracker that has '_writeToCurOp' set -- it never affects what
    // is propagated to '_base'.
    //
    // Kept adjacent to the counters above rather than after '_writeToCurOp' so that every field
    // touched by the addInternal() hot path lies within the first bytes of the object.
    int64_t _curOpWriteChunkSize;

    // Last lower-bound chunk reported to CurOp. -1 indicates that no chunk has been reported yet.
    int64_t _lastReportedLowerBound = -1;

    MemoryUsageLimit _maxAllowedMemoryUsageBytes;

    // The figure the limit, peak and CurOp reports use: unique for the operation tracker, else
    // full. The two counters are never made equal; read 'inUseUniqueMemoryBytes()' for the unique.
    int64_t limitFigure() const {
        return _isOperationTracker ? _inUseUniqueMemoryBytes : _inUseFullMemoryBytes;
    }

    // Allow for some extra bookkeeping to be done when add() is called. If set, this function will
    // be invoked with limitFigure() and _peakTrackedMemoryBytes. This mechanism exists
    // to avoid making add() virtual, since it has been shown to have an effect on performance in
    // some cases.
    std::function<void(int64_t, int64_t)> _writeToCurOp;
};

/**
 * This is a utility class for tracking memory usage across multiple arbitrary operators or
 * functions, which are identified by their string names. Tracks both current and highest
 * encountered memory consumption.
 *
 * It can be used directly by calling MemoryUsageTracker::add(int64_t fullDiff, int64_t uniqueDiff),
 * or by creating a dependent tracker via MemoryUsageTracker::operator[].
 *
 * Dependent tracker will update both its own memory and the total. It is used to track the
 * consumption of individual parts, such as different accumulators in $group, while simultaneously
 * keeping track of the total.
 *
 * Cannot be shallow copied because child memory trackers point to the address of the inline
 * base tracker of the class.
 *
 * TODO SERVER-113197: Remove streams dependency on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MemoryUsageTracker {
public:
    MemoryUsageTracker(const MemoryUsageTracker&) = delete;
    MemoryUsageTracker operator=(const MemoryUsageTracker&) = delete;

    MemoryUsageTracker(MemoryUsageTracker&&) = default;
    MemoryUsageTracker& operator=(MemoryUsageTracker&&) = default;

    MemoryUsageTracker(SimpleMemoryUsageTracker* baseParent,
                       bool allowDiskUse = false,
                       MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{0});

    MemoryUsageTracker(bool allowDiskUse = false,
                       MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{0});

    /**
     * Sets the new total for 'name', and updates the current total memory usage.
     */
    void set(std::string_view name, int64_t fullTotal, int64_t uniqueTotal);
    void set(std::string_view name, int64_t total) {
        set(name, total, total);
    }

    /**
     * Resets both the total memory usage as well as the per-function memory usage, but retains the
     * current value for maximum total memory usage.
     */
    void resetCurrent();

    /**
     * Clears the child memory trackers map and resets the base tracker memory usage to zero.
     */
    void clear();

    /**
     * Non-const version, creates a new element if one doesn't exist and returns a reference to it.
     */
    SimpleMemoryUsageTracker& operator[](std::string_view name);

    /**
     * Updates the memory usage for 'name' by adding 'fullDiff' and 'uniqueDiff' to the current
     * memory usage for that function. Also updates the total memory usage.
     */
    void add(std::string_view name, int64_t fullDiff, int64_t uniqueDiff);
    void add(std::string_view name, int64_t diff) {
        add(name, diff, diff);
    }

    /**
     * Updates total memory usage.
     */
    void add(int64_t fullDiff, int64_t uniqueDiff) {
        _baseTracker.add(fullDiff, uniqueDiff);
    }
    void add(int64_t diff) {
        _baseTracker.add(diff, diff);
    }

    auto inUseTrackedMemoryBytes() const {
        return _baseTracker.inUseTrackedMemoryBytes();
    }
    auto peakTrackedMemoryBytes() const {
        return _baseTracker.peakTrackedMemoryBytes();
    }

    int64_t peakTrackedMemoryBytes(std::string_view name) const;

    bool withinMemoryLimit(OperationContext* opCtx) const {
        return _baseTracker.withinMemoryLimit(opCtx);
    }

    bool allowDiskUse() const {
        return _allowDiskUse;
    }

    void assertCanSpill(std::string_view name) const;

    int64_t maxAllowedMemoryUsageBytes(OperationContext* opCtx) const {
        return _baseTracker.maxAllowedMemoryUsageBytes(opCtx);
    }

    /**
     * Returns a new MemoryUsageTracker. The copy constructor for this class is purposefully
     * deleted - use this method instead. Note that the function memory tracker table will be
     * initialized as empty.
     */
    MemoryUsageTracker makeFreshMemoryUsageTracker() const;

private:
    bool _allowDiskUse;
    // Tracks current memory used. This tracker rolls up memory usage from all trackers in the
    // function memory tracker table.
    SimpleMemoryUsageTracker _baseTracker;
    // Tracks memory consumption per function using the output field name as a key.
    stdx::unordered_map<std::string, SimpleMemoryUsageTracker> _functionMemoryTracker;
};

/**
 * This is a utility class for tracking memory usage for record deduplication across multiple
 * arbitrary operators. Optionally, it can be used to report the metrics to the serverStatus
 * command.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] DeduplicatorReporter {
public:
    DeduplicatorReporter(const DeduplicatorReporter&) = delete;
    DeduplicatorReporter& operator=(const DeduplicatorReporter&) = delete;

    DeduplicatorReporter(DeduplicatorReporter&&) noexcept = default;
    DeduplicatorReporter& operator=(DeduplicatorReporter&&) noexcept = default;

    DeduplicatorReporter(std::function<void(int64_t, int64_t)> callback,
                         int64_t serverStatusChunkSize);

    void add(int64_t bytesDiff, int64_t recordsDiff = 1);

private:
    // Tracks the current memory footprint.
    int64_t _inUseTrackedMemoryBytes = 0;
    int64_t _inUseRecordIdCount = 0;

    // Memory usage updates will only be written to serverStatus if the usage surpasses this size.
    // Writing to serverStatus involves lock contention, so in performance-sensitive situations, we
    // should set a non-one size. If 1, no chunking is performed.
    int64_t _serverStatusChunkSize;
    // Last lower-bound chunk reported to serverStatus. Same as _inUseTrackedMemoryBytes and
    // _inUseRecordIdCount if _serverStatusChunkSize is 1 (no chunking).
    int64_t _lastReportedLowerBound = 0;
    int64_t _lastReportedRecordIdCount = 0;

    // Allow for some extra bookkeeping to be done when add() is called. If set, this function will
    // be invoked with _inUseTrackedMemoryBytes and _inUseRecordIdCount.
    std::function<void(int64_t, int64_t)> _reportCallback;
};

/**
 * TODO SERVER-113197: Remove streams dependency on this class.
 */
template <typename Tracker>
class [[MONGO_MOD_OPEN]] MemoryUsageTokenImpl : private boost::noncopyable {
public:
    // Default constructor is only present to support ease of use for some containers.
    MemoryUsageTokenImpl() {}

    // Single-figure form: both figures are the same.
    MemoryUsageTokenImpl(size_t initial, Tracker* tracker)
        : MemoryUsageTokenImpl(initial, tracker, initial) {}

    MemoryUsageTokenImpl(size_t initialFull, Tracker* tracker, size_t initialUnique)
        : _tracker(tracker),
          _curMemoryUsageBytes(initialFull),
          _curUniqueMemoryBytes(initialUnique) {
        _tracker->add(_curMemoryUsageBytes, _curUniqueMemoryBytes);
    }

    MemoryUsageTokenImpl(MemoryUsageTokenImpl&& other)
        : _tracker(other._tracker),
          _curMemoryUsageBytes(other._curMemoryUsageBytes),
          _curUniqueMemoryBytes(other._curUniqueMemoryBytes) {
        other._tracker = nullptr;
    }

    MemoryUsageTokenImpl& operator=(MemoryUsageTokenImpl&& other) {
        if (this == &other) {
            return *this;
        }

        releaseMemory();
        _tracker = other._tracker;
        _curMemoryUsageBytes = other._curMemoryUsageBytes;
        _curUniqueMemoryBytes = other._curUniqueMemoryBytes;
        other._tracker = nullptr;
        return *this;
    }

    ~MemoryUsageTokenImpl() {
        releaseMemory();
    }

    int64_t getCurrentMemoryUsageBytes() const {
        return _curMemoryUsageBytes;
    }

    int64_t getCurrentUniqueMemoryBytes() const {
        return _curUniqueMemoryBytes;
    }

    void add(int64_t fullDiff, int64_t uniqueDiff) {
        if (!_tracker) {
            return;
        }

        _curMemoryUsageBytes += fullDiff;
        _curUniqueMemoryBytes += uniqueDiff;
        _tracker->add(fullDiff, uniqueDiff);
    }

    void set(int64_t fullTotal, int64_t uniqueTotal) {
        add(fullTotal - _curMemoryUsageBytes, uniqueTotal - _curUniqueMemoryBytes);
    }

    // Single-figure forms charge the same bytes to both figures.
    void add(int64_t diff) {
        add(diff, diff);
    }

    void set(int64_t total) {
        set(total, total);
    }

    const Tracker* tracker() const {
        return _tracker;
    }

    Tracker* tracker() {
        return _tracker;
    }

protected:
    void releaseMemory() {
        if (_tracker) {
            _tracker->add(-_curMemoryUsageBytes, -_curUniqueMemoryBytes);
        }
    }

    Tracker* _tracker{nullptr};
    int64_t _curMemoryUsageBytes{0};
    // The part of '_curMemoryUsageBytes' not already charged at its allocation.
    int64_t _curUniqueMemoryBytes{0};
};

using MemoryUsageToken = MemoryUsageTokenImpl<SimpleMemoryUsageTracker>;
using SimpleMemoryUsageToken = MemoryUsageTokenImpl<SimpleMemoryUsageTracker>;

/**
 * Template to easy couple MemoryTokens with stored data.
 */
template <typename Tracker, typename T>
class MemoryUsageTokenWithImpl {
public:
    template <std::enable_if_t<std::is_default_constructible_v<T>, bool> = true>
    MemoryUsageTokenWithImpl() : _token(), _value() {}

    template <typename... Args>
    MemoryUsageTokenWithImpl(MemoryUsageTokenImpl<Tracker> token, Args&&... args)
        : _token(std::move(token)), _value(std::forward<Args>(args)...) {}

    MemoryUsageTokenWithImpl(const MemoryUsageTokenWithImpl&) = delete;
    MemoryUsageTokenWithImpl& operator=(const MemoryUsageTokenWithImpl&) = delete;

    MemoryUsageTokenWithImpl(MemoryUsageTokenWithImpl&& other) = default;
    MemoryUsageTokenWithImpl& operator=(MemoryUsageTokenWithImpl&& other) = default;

    const T& value() const {
        return _value;
    }
    T& value() {
        return _value;
    }

private:
    MemoryUsageTokenImpl<Tracker> _token;
    T _value;
};

template <typename T>
using MemoryUsageTokenWith = MemoryUsageTokenWithImpl<SimpleMemoryUsageTracker, T>;

template <typename T>
using SimpleMemoryUsageTokenWith = MemoryUsageTokenWithImpl<SimpleMemoryUsageTracker, T>;

}  // namespace mongo
