import {canonicalizeEventForTesting} from "jstests/libs/query/change_stream_util.js";
import {kEndOfTransactionOperationType} from "jstests/libs/util/change_stream/change_stream_commands.js";

function computeTxnId(event) {
    return event.lsid && event.txnNumber !== undefined
        ? `${event.lsid.id ?? event.lsid}|${event.txnNumber}`
        : null;
}

/**
 * Filter event to only contain keys present in expected.
 * Enables subset matching by keeping only the fields we care about.
 * @param {Object} event - The actual event to filter.
 * @param {Object} expected - The expected event (defines which keys to keep).
 * @returns {Object} Filtered event with only expected keys.
 */
function filterToExpectedKeys(event, expected) {
    // First apply standard canonicalization.
    canonicalizeEventForTesting(event, expected);

    // Keep only keys that exist in expected.
    const filtered = {};
    for (const key in expected) {
        if (event.hasOwnProperty(key)) {
            filtered[key] = event[key];
        }
    }
    return filtered;
}

/**
 * Simple change event matcher used by tests.
 * Uses static eventModifier property to transform actual event before comparison.
 * Default: filterToExpectedKeys (removes extra fields, keeps only expected keys).
 *
 * To use custom filtering, set ChangeEventMatcher.eventModifier before creating matchers:
 *   ChangeEventMatcher.eventModifier = myCustomFilter;
 */
/**
 * CommittedTxn: the expected-domain envelope a CommitTxnCommand returns from
 * getChangeEvents(). It is NOT an event and carries no operationType — never
 * handed raw to a ChangeEventMatcher — the matcher layer unwinds it. `events`
 * are the txn's in-scope statement events (statement order) followed by the
 * endOfTransaction expected event when the txn's covered scopes include the watched
 * scope.
 */
class CommittedTxn {
    constructor(events) {
        this.events = events;
    }
}

class ChangeEventMatcher {
    /**
     * Static event modifier function. Override for custom filtering.
     * Default: filterToExpectedKeys.
     */
    static eventModifier = filterToExpectedKeys;

    /**
     * @param {Object} event - Expected change event.
     * @param {boolean} cursorClosed - Expected cursorClosed state.
     */
    constructor(event, cursorClosed = false) {
        this._event = event;
        this._cursorClosed = cursorClosed;
        this._consumed = false;
    }

    /**
     * Check if the provided event matches the expected values.
     * Uses static eventModifier to filter/transform actual event before comparison.
     * @param {Object} event - Actual change event.
     * @param {boolean} cursorClosed - Actual cursorClosed state.
     * @returns {boolean} True if events match, false otherwise.
     */
    matches(event, cursorClosed = false) {
        if (this.isDone()) {
            return false;
        }

        if (this._cursorClosed !== cursorClosed) {
            return false;
        }

        const actualFiltered = ChangeEventMatcher.eventModifier(
            Object.assign({}, event),
            this._event,
        );

        // Use bsonUnorderedFieldsCompare to ignore field order (returns 0 if equal)
        this._consumed = bsonUnorderedFieldsCompare(actualFiltered, this._event) === 0;
        return this._consumed;
    }

    getMatchedCount() {
        return this._consumed ? 1 : 0;
    }

    isDone() {
        return this._consumed;
    }

    getTotalCount() {
        return 1;
    }

    /**
     * Whether the stream may skip over this element when a later element matches
     * (matchesOrSkip). Plain events are skippable; a txn matcher is position-locked.
     */
    skippable() {
        return true;
    }

    toString() {
        return this._event;
    }
}

/**
 * EotMode: how a TxnChangeEventMatcher treats the endOfTransaction arm.
 * - kRequired: the EOT must arrive after the to-be-matched set drains (the default).
 * - kWhenAvailable: the EOT may never arrive; the matcher completes once the set drains.
 * - kOff: no EOT arm (showEndOfTransaction: off).
 */
const EotMode = {
    kRequired: "required",
    kWhenAvailable: "whenAvailable",
    kOff: "off",
};

/**
 * TxnChangeEventMatcher: the txn counterpart of ChangeEventMatcher — it matches one
 * committed transaction's expected events. Holds toBeMatchedTxnEvents — the txn's
 * statement events not yet matched (order-free: a match looks the event up by
 * content and removes it; a miss returns false) — plus an EOT arm that refuses
 * while the set is non-empty (EOT-last, enforced at match time). Txn events are
 * delivered once (no stream-level dedup); a completed matcher is closed.
 */
class TxnChangeEventMatcher {
    constructor(committedTxn, {eotMode = EotMode.kRequired} = {}) {
        const nonEotEvents = committedTxn.events.filter(
            (event) => event.operationType !== kEndOfTransactionOperationType,
        );
        this.toBeMatchedTxnEvents = nonEotEvents.map((event) => new ChangeEventMatcher(event));

        let eotEvent = null;
        for (const ev of committedTxn.events) {
            if (ev.operationType === kEndOfTransactionOperationType) {
                eotEvent = ev;
            }
        }
        this._eotMode = eotMode;
        this._eotMatcher =
            eotEvent !== null && eotMode !== EotMode.kOff ? new ChangeEventMatcher(eotEvent) : null;
        this._eotConsumed = false;

        this._matchedCount = 0;
        this._totalCount = this.toBeMatchedTxnEvents.length + (this._eotMatcher !== null ? 1 : 0);

        // NOTE: 'txnNumber' and 'lsid' are runtime properties and is not present in the events. Therefore it
        // will be lazily assigned on the first event match.
        this._txnId = null;
        assert(
            this.toBeMatchedTxnEvents.length > 0 || this._eotMatcher !== null,
            "TxnChangeEventMatcher requires at least one expected event",
        );
    }

    matches(event, cursorClosed) {
        if (this.isDone()) {
            return false;
        }

        // Identity gate: once bound, only this txn's events participate.
        if (this._txnId && computeTxnId(event) !== this._txnId) {
            return false;
        }

        // 1. The to-be-matched set: a match looks the event up by content and
        // removes it.
        const matchingEventIdx = this.toBeMatchedTxnEvents.findIndex((m) =>
            m.matches(event, cursorClosed),
        );
        if (matchingEventIdx !== -1) {
            this.toBeMatchedTxnEvents.splice(matchingEventIdx, 1);
            this._matchedCount++;

            // Lazy-bind the txn identity from the first consumed statement event.
            if (!this._txnId) {
                this._txnId = computeTxnId(event);
            }
            return true;
        }

        // 2. The EOT arm: only after the to-be-matched set drains (EOT-last).
        if (
            this.toBeMatchedTxnEvents.length === 0 &&
            this._eotMatcher !== null &&
            this._eotMatcher.matches(event, cursorClosed)
        ) {
            this._eotConsumed = true;
            this._matchedCount++;
            return true;
        }

        return false;
    }

    isDone() {
        if (this._eotMatcher === null || this._eotMode === EotMode.kWhenAvailable) {
            return this.toBeMatchedTxnEvents.length === 0 || this._eotConsumed;
        }
        return this._eotConsumed;
    }

    getMatchedCount() {
        return this._matchedCount;
    }

    getTotalCount() {
        return this._totalCount;
    }

    toString() {
        return `CommittedTxn({txnId: ${this.getTxnId()}, eventsToBeMatched: ${tojsononeline(this.toBeMatchedTxnEvents.map((m) => m.toString()))}, didMatchEOT: ${this._eotConsumed}})`;
    }

    getTxnId() {
        return this._txnId;
    }

    skippable() {
        // A txn matcher NO event has arrived for may be skipped past — whether its
        // events will ever arrive is unknowable to the matcher (e.g. its shard was
        // removed: change stream read time differs from shard removal time). Once it
        // has consumed events it is position-locked: the stream never skips over a
        // partially-matched txn.
        return this.getMatchedCount() === 0;
    }
}

export {
    ChangeEventMatcher,
    TxnChangeEventMatcher,
    CommittedTxn,
    EotMode,
    filterToExpectedKeys,
    computeTxnId,
};
