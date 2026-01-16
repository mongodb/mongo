/**
 * Change stream matcher infrastructure.
 * Handles ordered and interleaved event stream matching.
 */

/**
 * SingleChangeStreamMatcher - Matches events from a single change stream.
 * Supports tolerant matching for per-shard events that can arrive out of order.
 */
class SingleChangeStreamMatcher {
    /**
     * @param {Array<ChangeEventMatcher>} eventMatchers - Ordered list of expected events
     */
    constructor(eventMatchers) {
        this.matchers = eventMatchers;
        this.index = 0;
        this.deferred = []; // Per-shard events that arrived out of order
    }

    /**
     * Consume deferred events that match subsequent expected events.
     *
     * In a sharded cluster, operations like createIndexes and dropIndexes execute
     * independently on each shard, emitting separate events. These per-shard events
     * may arrive interleaved with subsequent command events due to async execution:
     *
     *   Expected: [drop, drop, create, shard]  (2 drops from 2 shards)
     *   Actual:   [drop, create, drop, shard]  (shard 2's drop arrives late)
     *
     * This method handles reordering by consuming deferred events that now match
     * the current expected position after a successful match advances the index.
     *
     * @param {boolean} cursorClosed - Whether the cursor has been closed
     */
    _consumeDeferred(cursorClosed) {
        let consumed = true;
        while (consumed && this.index < this.matchers.length && this.deferred.length > 0) {
            consumed = false;
            const matcher = this.matchers[this.index];
            for (let i = 0; i < this.deferred.length; i++) {
                if (matcher.matches(this.deferred[i], cursorClosed)) {
                    this.deferred.splice(i, 1);
                    this.index++;
                    consumed = true;
                    break;
                }
            }
        }
    }

    /**
     * Process a single change event.
     * If matched, advances and tries to consume any deferred per-shard events.
     * If not matched, defers the event (may be a per-shard event arriving early).
     * @param {Object} event - The change event to process
     * @param {boolean} cursorClosed - Whether the cursor has been closed
     * @returns {boolean} True if the event matched, false otherwise
     */
    matches(event, cursorClosed) {
        if (this.isDone()) {
            return false; // Extra event beyond expected
        }
        const matcher = this.matchers[this.index];
        if (matcher.matches(event, cursorClosed)) {
            this.index++;
            this._consumeDeferred(cursorClosed);
            return true;
        }
        // Didn't match current expected - may be a per-shard event arriving early
        this.deferred.push(event);
        return false;
    }

    /**
     * Assert that the next expected event matches the provided event.
     * @param {Object} event - The change event to process
     * @param {boolean} cursorClosed - Whether the cursor has been closed
     */
    assertMatches(event, cursorClosed) {
        assert(this.index < this.matchers.length, "No more expected events");
        const matcher = this.matchers[this.index];
        assert(matcher.matches(event, cursorClosed), `Event mismatch at index ${this.index}`);
        this.index++;
    }

    /**
     * Check if all expected events have been matched.
     * Tries to consume any remaining deferred events before checking.
     * @returns {boolean} True if all events matched
     */
    isDone() {
        this._consumeDeferred(false);
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
