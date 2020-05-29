'use strict';

/* Read the contents of /proc/net/netstat, and transform the output into a JS object.
 * Netstat contains sections of key/value pairs, for different metrics tracked by the
 * Linux kernel. Sections are encoded as two lines, prefixed by the field name. The first
 * line contains an ordered list of all key names. The second line contains the ordered
 * list of values.
 * The schema of resulting Javascript object is:
 * {
 *   "section1": {
 *     "key1": value1,
 *     "key2": value2,
 *     ...
 *   },
 *   "section2": {
 *     "key1": value1,
 *     ...
 *   },
 *   ...
 * }
 */
function getNetStatObj() {
    return cat("/proc/net/netstat")
        .split("\n")
        .filter((item) => item.length)
        .map(line => line.split(" "))
        .reduce((acc, current) => {
            const sectionName = current[0].slice(0, -1);
            // If we're populating a subsection for the first time,
            // just copy over the key names, resulting in:
            // {..., "section": ["key1", "key2", ..., "keyN"] }
            if (!acc[sectionName]) {
                acc[sectionName] = current.slice(1);
            } else {
                // Merge the values into a subsection, resulting in:
                // {..., "section": {"key1": value1, ..., "keyN", valueN} }
                const populated = {};
                acc[sectionName].forEach((item, index) => {
                    populated[item] = current[index + 1];
                });
                acc[sectionName] = populated;
            }
            return acc;
        }, {});
}
