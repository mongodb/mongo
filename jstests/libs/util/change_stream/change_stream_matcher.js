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
     * @param {Array<ChangeEventMatcher>} eventMatchers - Ordered list of expected events
     */
    constructor(eventMatchers) {
        this.matchers = eventMatchers;
        this.index = 0;
        this.skipped = [];
    }

    matches(event, cursorClosed) {
        if (
            this.index < this.matchers.length &&
            this.matchers[this.index].matches(event, cursorClosed)
        ) {
            this.index++;
            return true;
        }
        return false;
    }

    /**
     * Match event against expected, skipping unmatched expected events.
     * Only modifies state on success — safe to call speculatively from
     * MultipleChangeStreamMatcher without save/restore.
     */
    matchesOrSkip(event, cursorClosed) {
        for (let i = this.index; i < this.matchers.length; i++) {
            if (this.matchers[i].matches(event, cursorClosed)) {
                while (this.index < i) {
                    this.skipped.push({
                        index: this.index,
                        type: this.matchers[this.index].event.operationType,
                    });
                    this.index++;
                }
                this.index = i + 1;
                return true;
            }
        }
        return false;
    }

    isDone() {
        return this.index === this.matchers.length;
    }

    getMatchedCount() {
        return this.index;
    }

    /**
     * Expected events as "operationType(ns)" strings, for debugging output. Namespace is
     * included since a composite (multi-collection) matcher's streams are otherwise
     * indistinguishable from their operation-type sequences alone.
     */
    getExpectedEventSummaries() {
        return [this.matchers.map((m) => formatEventTypeAndNs(m.event.operationType, m.event.ns))];
    }

    /**
     * This stream's own matched/total/done state, plus the last event it consumed and the next
     * one it's waiting for, for debugging output.
     */
    getPerStreamBreakdown() {
        return [
            {
                matched: this.index,
                total: this.matchers.length,
                done: this.isDone(),
                lastMatched:
                    this.index > 0
                        ? formatEventTypeAndNs(
                              this.matchers[this.index - 1].event.operationType,
                              this.matchers[this.index - 1].event.ns,
                          )
                        : null,
                nextExpected: this.isDone()
                    ? null
                    : formatEventTypeAndNs(
                          this.matchers[this.index].event.operationType,
                          this.matchers[this.index].event.ns,
                      ),
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
