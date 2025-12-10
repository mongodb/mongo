import {canonicalizeEventForTesting} from "jstests/libs/query/change_stream_util.js";

/**
 * Simple change event matcher used by tests.
 * Compares only the fields specified in the expected event, ignoring
 * resume token / clusterTime / unknowable fields unless explicitly provided.
 */
class ChangeEventMatcher {
    /**
     * @param {Object} event - Expected change event.
     * @param {boolean} cursorClosed - Expected cursorClosed state.
     */
    constructor(event, cursorClosed = false) {
        this.event = event;
        this.cursorClosed = cursorClosed;
    }

    /**
     * Check if the provided event and cursorClosed flag match the expected values.
     * @param {Object} event - Actual change event.
     * @param {boolean} cursorClosed - Actual cursorClosed state.
     * @returns {boolean} True if events match, false otherwise.
     */
    matches(event, cursorClosed = false) {
        if (this.cursorClosed !== cursorClosed) {
            return false;
        }
        const actualCanon = canonicalizeEventForTesting(event, this.event);
        return friendlyEqual(actualCanon, this.event);
    }
}

export {ChangeEventMatcher};
