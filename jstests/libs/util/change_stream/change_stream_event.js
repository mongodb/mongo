import {canonicalizeEventForTesting} from "jstests/libs/query/change_stream_util.js";

/**
 * Simple change event matcher used by tests.
 * Uses static eventModifier property to transform actual event before comparison.
 * Default: canonicalizeEventForTesting (removes _id, clusterTime, etc.).
 *
 * To use custom filtering, set ChangeEventMatcher.eventModifier before creating matchers:
 *   ChangeEventMatcher.eventModifier = myCustomFilter;
 */
class ChangeEventMatcher {
    /**
     * Static event modifier function. Override for custom filtering.
     * Default: canonicalizeEventForTesting
     */
    static eventModifier = canonicalizeEventForTesting;

    /**
     * @param {Object} event - Expected change event.
     * @param {boolean} cursorClosed - Expected cursorClosed state.
     */
    constructor(event, cursorClosed = false) {
        this.event = event;
        this.cursorClosed = cursorClosed;
    }

    /**
     * Check if the provided event matches the expected values.
     * Uses static eventModifier to filter/transform actual event before comparison.
     * @param {Object} event - Actual change event.
     * @param {boolean} cursorClosed - Actual cursorClosed state.
     * @returns {boolean} True if events match, false otherwise.
     */
    matches(event, cursorClosed = false) {
        if (this.cursorClosed !== cursorClosed) {
            return false;
        }
        const actualCanon = ChangeEventMatcher.eventModifier(Object.assign({}, event), this.event);
        return friendlyEqual(actualCanon, this.event);
    }
}

export {ChangeEventMatcher};
