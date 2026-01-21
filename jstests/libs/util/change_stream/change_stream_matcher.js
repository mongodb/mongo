/**
 * Change stream matcher infrastructure.
 * Handles ordered and interleaved event stream matching.
 */

/**
 * SingleChangeStreamMatcher - Matches events from a single change stream.
 * Supports tolerant matching for per-shard events that can arrive out of order.
 *
 * TODO SERVER-117490: The deferred matching logic here handles out-of-order per-shard events
 * (e.g., createIndexes, dropIndexes) that can arrive interleaved with other events due to
 * async execution across shards. Callers should use assertDone() after processing all events
 * to verify all expected events were matched and no extra events were received.
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
     * Check if all expected events have been matched.
     * Tries to consume any remaining deferred events before checking.
     * @returns {boolean} True if all events matched
     */
    isDone() {
        this._consumeDeferred(false);
        return this.index === this.matchers.length;
    }

    /**
     * Check for unmatched expected events and fail with details if any.
     */
    _assertAllExpectedMatched() {
        if (this.index >= this.matchers.length) {
            return;
        }
        const unmatched = [];
        for (let i = this.index; i < this.matchers.length; i++) {
            unmatched.push({index: i, type: this.matchers[i].event.operationType});
        }
        jsTest.log.info(`=== MATCHING FAILURE: Missing expected events ===`);
        jsTest.log.info(`  Expected: ${this.matchers.length}, Matched: ${this.index}`);
        jsTest.log.info(`  Unmatched expected: ${tojson(unmatched)}`);
        if (this.deferred.length > 0) {
            jsTest.log.info(`  Deferred (unmatched actual): ${this.deferred.map((e) => e.operationType).join(", ")}`);
        }
        assert(
            false,
            `Not all events matched. Matched ${this.index} of ${this.matchers.length}. ` +
                `Unmatched: ${tojson(unmatched)}`,
        );
    }

    /**
     * Check for extra unexpected events and fail with details if any.
     */
    _assertNoExtraEvents() {
        if (this.deferred.length === 0) {
            return;
        }
        jsTest.log.info(`=== MATCHING FAILURE: Unexpected extra events ===`);
        jsTest.log.info(`  Expected: ${this.matchers.length}, Matched: ${this.index}`);
        jsTest.log.info(`  Extra events: ${this.deferred.map((e) => e.operationType).join(", ")}`);
        assert(
            false,
            `Received ${this.deferred.length} unexpected extra events: ` +
                `[${this.deferred.map((e) => e.operationType).join(", ")}]`,
        );
    }

    /**
     * Assert that all expected events have been matched and no extra events received.
     */
    assertDone() {
        this._consumeDeferred(false);
        this._assertAllExpectedMatched();
        this._assertNoExtraEvents();
    }

    /**
     * Peek at the next expected event without advancing.
     * @returns {Object|null} The expected event object, or null if done
     */
    peekExpected() {
        if (this.isDone()) {
            return null;
        }
        return this.matchers[this.index].event;
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
