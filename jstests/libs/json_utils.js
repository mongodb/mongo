// JSON-related utility functions.

/**
 * Makes a deep copy of the provided JSON.
 */
export function copyJSON(json) {
    return JSON.parse(JSON.stringify(json));
}