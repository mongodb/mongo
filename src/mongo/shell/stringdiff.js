/**
 * Compares two strings and returns their differences in patch format.
 *
 * This function uses Myers diff algorithm to compute the differences between two strings,
 * then converts the result into a patch format that can be applied to transform the old
 * string into the new string.
 *
 * @param {string} oldStr - The original string to compare from
 * @param {string} newStr - The new string to compare to
 * @returns {*} A patch representation of the differences between oldStr and newStr
 * @throws {AssertionError} If oldStr is not a string
 * @throws {AssertionError} If newStr is not a string
 *
 * @example
 * const diff = stringdiff("hello world", "hello javascript");
 * // Returns a patch showing the transformation from "hello world" to "hello javascript"
 */
export function stringdiff(oldStr, newStr) {
    assert(typeof oldStr === "string");
    assert(typeof newStr === "string");

    return patchdiff(myersdiff(oldStr, newStr));
}

const INS = "+";
const DEL = "-";
const PAD = " "; // matching lines
const SEP = "---";

/**
 * Converts a full diff output into a patch format with context windows.
 *
 * This function processes a diff string (with lines prefixed by '+', '-', or ' ')
 * and returns a condensed version showing only the changed lines plus a configurable
 * number of surrounding context lines. Separate chunks of changes are delimited with '---'.
 *
 * @param {string} fulldiff - The complete diff string with each line prefixed by '+' (insertion),
 *                            '-' (deletion), or ' ' (unchanged)
 * @returns {string} A condensed patch showing only changed lines with 4 lines of context
 *                   before and after each change. Separate change chunks are separated by '---'.
 *
 * @example
 * const fulldiff = " line1\n line2\n-line3\n+line3a\n line4\n line5";
 * const patch = patchdiff(fulldiff);
 * // Returns: " line1\n line2\n-line3\n+line3a\n line4\n line5"
 *
 * @example
 * // With large gaps between changes, chunks are separated
 * const fulldiff = "-a\n+b\n c\n d\n e\n f\n g\n h\n i\n j\n-k\n+l";
 * const patch = patchdiff(fulldiff);
 * // Returns: "-a\n+b\n c\n d\n e\n f\n---\n g\n h\n i\n j\n-k\n+l"
 */
function patchdiff(fulldiff) {
    let lines = fulldiff.split("\n");

    const context = 4; // surround with 4 lines for context before/after diff

    let keep = [];
    for (let i = 0; i < lines.length; i++) {
        if (lines[i].startsWith(DEL) || lines[i].startsWith(INS)) {
            let start = Math.max(0, i - context);
            let end = Math.min(lines.length, i + context + 1);
            for (let j = start; j < end; j++) {
                keep[j] = true;
            }
        }
    }

    let result = [];
    for (let i = 0; i < lines.length; i++) {
        if (keep[i]) {
            if (i > 0 && !keep[i - 1] && result.length > 0) {
                result.push(SEP);
            }
            result.push(lines[i]);
        }
    }
    result = result.join("\n");
    return result;
}
/**
 * Implements Myers diff algorithm to compute the difference between two strings.
 *
 * This function uses Myers' O(ND) difference algorithm to find the shortest edit script
 * that transforms string `a` into string `b`. The algorithm splits both strings into lines
 * and computes insertions, deletions, and unchanged sections.
 *
 * The result is a multi-line string where each line is prefixed with:
 * - ' ' (space) for unchanged lines
 * - '-' for lines deleted from the original string
 * - '+' for lines added in the new string
 *
 * @param {string} a - The original string to compare from
 * @param {string} b - The new string to compare to
 * @returns {string} A diff string with each line prefixed by ' ', '-', or '+' indicating
 *                   unchanged, deleted, or inserted lines respectively. Lines are separated
 *                   by newline characters.
 *
 * @example
 * const diff = myersdiff("hello\nworld", "hello\njavascript");
 * // Returns: " hello\n-world\n+javascript"
 *
 * @see {@link https://blog.jcoglan.com/2017/02/12/the-myers-diff-algorithm-part-1/|Myers Diff Algorithm}
 */
function myersdiff(a, b) {
    const aLines = a.split("\n");
    const bLines = b.split("\n");

    const N = aLines.length;
    const M = bLines.length;
    const MAX = N + M;

    const v = [];
    const trace = [];
    v[1] = 0;

    for (let d = 0; d <= MAX; d++) {
        trace.push({...v});

        for (let k = -d; k <= d; k += 2) {
            let x;
            if (k === -d || (k !== d && v[k - 1] < v[k + 1])) {
                x = v[k + 1];
            } else {
                x = v[k - 1] + 1;
            }

            let y = x - k;

            // Follow diagonal
            while (x < N && y < M && aLines[x] === bLines[y]) {
                x++;
                y++;
            }

            v[k] = x;
            if (x >= N && y >= M) {
                // Found the solution, now backtrack to build the diff
                return backtrack(aLines, bLines, trace, d);
            }
        }
    }

    return "";
}

function backtrack(aLines, bLines, trace, d) {
    let x = aLines.length;
    let y = bLines.length;
    const diff = [];

    for (let depth = d; depth >= 0; depth--) {
        const v = trace[depth];
        const k = x - y;

        let prevK = k;
        if (k === -depth || (k !== depth && v[k - 1] < v[k + 1])) {
            prevK++;
        } else {
            prevK--;
        }

        const prevX = v[prevK];
        const prevY = prevX - prevK;

        // Add diagonal (unchanged) lines
        while (x > prevX && y > prevY) {
            x--;
            y--;
            diff.unshift(PAD + aLines[x]);
        }

        // Add deletion or insertion
        if (depth > 0) {
            if (x === prevX) {
                // Insertion
                y--;
                diff.unshift(INS + bLines[y]);
            } else {
                // Deletion
                x--;
                diff.unshift(DEL + aLines[x]);
            }
        }
    }

    return diff.join("\n");
}

const green = (s) => `\x1b[32m${s}\x1b[0m`;
const red = (s) => `\x1b[31m${s}\x1b[0m`;

/**
 * Decorates a patch diff with colorized lines.
 * @param {string} diff
 * @returns {string}
 */
export function colorize(diff) {
    return diff
        .split("\n")
        .map((line) => {
            if (line.startsWith(INS)) {
                return green(line);
            } else if (line.startsWith(DEL) && line != SEP) {
                return red(line);
            }
            return line;
        })
        .join("\n");
}
