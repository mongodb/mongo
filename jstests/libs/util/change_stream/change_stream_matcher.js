/**
 * Change stream matcher infrastructure.
 * Strict sequential matching: events must arrive in exactly the predicted order.
 */

/**
 * Format "operationType(ns)" for debugging output. Namespace is included since a mismatch.
 * @param {string} operationType
 * @param {Object} [ns] - {db, coll}
 */
function formatEventTypeAndNs(operationType, ns) {
    const nsStr = ns ? (ns.coll ? `${ns.db}.${ns.coll}` : ns.db) : "?";
    return `${operationType}(${nsStr})`;
}

/**
 * SingleChangeStreamMatcher - Matches events from a single change stream in strict order.
 */
class SingleChangeStreamMatcher {
    /**
     * @param {Array} eventMatchers - Ordered list of matchers (ChangeEventMatcher |
     *     TxnChangeEventMatcher); a TxnChangeEventMatcher consumes a whole txn's event
     *     set, order-free inside.
     */
    constructor(eventMatchers) {
        this.matchers = eventMatchers;
        this.index = 0;
        this.skipped = [];
    }

    // A finished matcher becomes transparent: the stream advances past it. The
    // advancement happens EAGERLY here and nowhere else, so a done matcher never
    // sits at the stream's front.
    _onSuccessfulMatch(matcher, event) {
        if (matcher.isDone()) {
            this.index++;
        }
    }

    matches(event, cursorClosed) {
        if (this.isDone()) {
            return false;
        }

        const matcher = this.matchers[this.index];
        if (!matcher.matches(event, cursorClosed)) {
            return false;
        }
        this._onSuccessfulMatch(matcher, event);
        return true;
    }

    /**
     * Match event against expected, skipping unmatched expected events.
     * Only modifies state on success — safe to call speculatively from
     * MultipleChangeStreamMatcher without save/restore.
     * An unfinished txn matcher at the scan front is position-locked: it alone may
     * match this event, never be skipped over.
     */
    matchesOrSkip(event, cursorClosed) {
        for (let i = this.index; i < this.matchers.length; i++) {
            const matcher = this.matchers[i];
            if (!matcher.skippable()) {
                // Position-locked: an ACTIVE txn matcher alone may match this event —
                // the stream never skips over a partially-matched txn.
                if (!matcher.matches(event, cursorClosed)) {
                    return false;
                }
                this._onSuccessfulMatch(matcher, event);
                return true;
            }

            if (!matcher.matches(event, cursorClosed)) {
                continue;
            }
            while (this.index < i) {
                this.skipped.push({
                    index: this.index,
                    type: this.matchers[this.index].toString(),
                });
                this.index++;
            }
            this._onSuccessfulMatch(matcher, event);
            return true;
        }
        return false;
    }

    isDone() {
        return this.index === this.matchers.length;
    }

    getMatchedCount() {
        return this.matchers.reduce((acc, eventMatcher) => acc + eventMatcher.getMatchedCount(), 0);
    }

    getTotalCount() {
        return this.matchers.reduce((acc, eventMatcher) => acc + eventMatcher.getTotalCount(), 0);
    }

    /**
     * Expected events as "operationType(ns)" strings, for debugging output. Namespace is
     * included since a composite (multi-collection) matcher's streams are otherwise
     * indistinguishable from their operation-type sequences alone.
     */
    getExpectedEventSummaries() {
        return [this.matchers.map((m) => m.toString())];
    }

    /**
     * This stream's own matched/total/done state, plus the last event it consumed and the next
     * one it's waiting for, for debugging output.
     */
    getPerStreamBreakdown() {
        const total = this.matchers.reduce((n, m) => n + m.getTotalCount(), 0);
        return [
            {
                matched: this.getMatchedCount(),
                total,
                done: this.isDone(),
                // The last element that consumed anything — an in-progress txn
                // matcher has matched events without advancing index, so the scan is
                // by consumed state, not by index position.
                lastMatched:
                    [...this.matchers]
                        .reverse()
                        .find((m) => m.getMatchedCount() > 0)
                        ?.toString() ?? null,
                nextExpected: this.isDone() ? null : this.matchers[this.index].toString(),
            },
        ];
    }
}

/**
 * MultipleChangeStreamMatcher - Matches events from multiple interleaved change streams.
 */
class MultipleChangeStreamMatcher {
    /**
     * @param {Array<SingleChangeStreamMatcher>} streamMatchers - List of stream matchers
     */
    constructor(streamMatchers) {
        this.matchers = streamMatchers;
    }

    /**
     * Process a single change event. Tries to match against any stream.
     * @param {Object} event - The change event to process
     * @param {boolean} cursorClosed - Whether cursors have been closed
     * @returns {boolean} True if the event matched any stream, false otherwise
     */
    matches(event, cursorClosed) {
        return this.matchers.some((matcher) => matcher.matches(event, cursorClosed));
    }

    matchesOrSkip(event, cursorClosed) {
        return this.matchers.some((matcher) => matcher.matchesOrSkip(event, cursorClosed));
    }

    /**
     * Check if all streams have matched all their expected events.
     * @returns {boolean} True if all streams are done, false otherwise
     */
    isDone() {
        return this.matchers.every((matcher) => matcher.isDone());
    }

    getMatchedCount() {
        return this.matchers.reduce((sum, m) => sum + m.getMatchedCount(), 0);
    }

    getExpectedEventSummaries() {
        return this.matchers.flatMap((m) => m.getExpectedEventSummaries());
    }

    getPerStreamBreakdown() {
        return this.matchers.flatMap((m) => m.getPerStreamBreakdown());
    }
}

export {SingleChangeStreamMatcher, MultipleChangeStreamMatcher, formatEventTypeAndNs};
