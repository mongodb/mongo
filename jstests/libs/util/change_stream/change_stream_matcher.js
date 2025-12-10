/**
 * Change stream matcher infrastructure.
 * Handles ordered and interleaved event stream matching.
 */

/**
 * SingleChangeStreamMatcher - Matches events from a single change stream.
 * Validates that events arrive in the expected order.
 */
class SingleChangeStreamMatcher {
    /**
     * @param {Array<ChangeEventMatcher>} eventMatchers - Ordered list of expected events
     */
    constructor(eventMatchers) {
        this.matchers = eventMatchers;
        this.index = 0;
    }

    /**
     * Process a single change event. Advances to the next matcher if matched.
     * @param {Object} event - The change event to process
     * @param {boolean} cursorClosed - Whether the cursor has been closed
     * @returns {boolean} True if the event matched, false otherwise
     */
    matches(event, cursorClosed) {
        if (this.isDone()) {
            return false;
        }
        const matcher = this.matchers[this.index];
        if (matcher.matches(event, cursorClosed)) {
            this.index++;
            return true;
        }
        return false;
    }

    /**
     * Assert that the next expected event matches the provided event.
     * @param {Object} event - The change event to process
     * @param {boolean} cursorClosed - Whether the cursor has been closed
     */
    assertMatches(event, cursorClosed) {
        assert(!this.isDone(), "No more expected events");
        const matcher = this.matchers[this.index];
        assert(matcher.matches(event, cursorClosed), `Event mismatch at index ${this.index}`);
        this.index++;
    }

    /**
     * Check if all expected events have been matched.
     * @returns {boolean} True if all events matched, false otherwise
     */
    isDone() {
        return this.index === this.matchers.length;
    }

    /**
     * Assert that all expected events have been matched.
     */
    assertDone() {
        assert(this.isDone(), `Not all events matched. Matched ${this.index} of ${this.matchers.length}`);
    }
}

/**
 * MultipleChangeStreamMatcher - Matches events from multiple interleaved change streams.
 * Events from different streams can arrive in any order, but per-stream order must be preserved.
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

    /**
     * Assert that the event matches at least one stream.
     * @param {Object} event - The change event to process
     * @param {boolean} cursorClosed - Whether cursors have been closed
     */
    assertMatches(event, cursorClosed) {
        assert(this.matches(event, cursorClosed), "Event did not match any stream");
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
}

export {SingleChangeStreamMatcher, MultipleChangeStreamMatcher};
