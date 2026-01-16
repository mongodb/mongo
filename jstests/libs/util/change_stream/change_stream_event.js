import {canonicalizeEventForTesting} from "jstests/libs/query/change_stream_util.js";

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
 * Uses subset matching via filterToExpectedKeys: only compares fields present in expected.
 *
 * Uses static eventModifier property to transform actual event before comparison.
 * Default: filterToExpectedKeys (keeps only expected keys after canonicalization).
 *
 * To use custom filtering, set ChangeEventMatcher.eventModifier before creating matchers:
 *   ChangeEventMatcher.eventModifier = myCustomFilter;
 */
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
        const actualFiltered = ChangeEventMatcher.eventModifier(Object.assign({}, event), this.event);
        return friendlyEqual(actualFiltered, this.event);
    }
}

export {ChangeEventMatcher, filterToExpectedKeys};
