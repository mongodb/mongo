/**
 * Change stream matcher infrastructure.
 * Strict sequential matching: events must arrive in exactly the predicted order.
 */

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
        this.mismatch = null;
        this.skipped = [];
    }

    matches(event, cursorClosed) {
        if (this.index >= this.matchers.length) {
            this.mismatch = {
                index: this.index,
                expected: "<end of expected>",
                actual: event.operationType,
            };
            return false;
        }
        if (this.matchers[this.index].matches(event, cursorClosed)) {
            this.index++;
            return true;
        }
        this.mismatch = {
            index: this.index,
            expected: this.matchers[this.index].event.operationType,
            actual: event.operationType,
        };
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

    assertDone() {
        assert(
            this.isDone(),
            this.mismatch
                ? `Event mismatch at index ${this.mismatch.index}: ` +
                      `expected '${this.mismatch.expected}', got '${this.mismatch.actual}'`
                : `Matched ${this.index} of ${this.matchers.length}`,
        );
    }

    getFirstMismatch() {
        return this.mismatch;
    }

    getMatchedCount() {
        return this.index;
    }

    getExpectedOperationTypes() {
        return [this.matchers.map((m) => m.event.operationType)];
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

    /**
     * Assert that all streams have matched all their expected events.
     */
    assertDone() {
        this.matchers.forEach((matcher, idx) => {
            assert(matcher.isDone(), `Stream ${idx} not done. Matched ${matcher.index} of ${matcher.matchers.length}`);
        });
    }

    getFirstMismatch() {
        for (const matcher of this.matchers) {
            const m = matcher.getFirstMismatch();
            if (m) {
                return m;
            }
        }
        return null;
    }

    getMatchedCount() {
        return this.matchers.reduce((sum, m) => sum + m.getMatchedCount(), 0);
    }

    getExpectedOperationTypes() {
        return this.matchers.flatMap((m) => m.getExpectedOperationTypes());
    }
}

export {SingleChangeStreamMatcher, MultipleChangeStreamMatcher};
