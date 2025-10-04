/**
 * Helper functions for timestamp validation.
 */

/**
 * Assert that the passed value is a BSON timestamp.
 */
export function isTimestamp(value) {
    return Object.prototype.toString.call(value) === "[object Timestamp]";
}
