/**
 * Provides helper functions to output content to markdown. This is used for golden testing, using
 * `printGolden` to write to the expected output files.
 */

let sectionCount = 1;
export function section(msg) {
    printGolden(`## ${sectionCount}.`, msg);
    sectionCount++;
}

export function subSection(msg) {
    printGolden("###", msg);
}

export function line(msg) {
    printGolden(msg);
}

export function codeOneLine(msg) {
    printGolden("`" + tojsononeline(msg) + "`");
}

export function code(msg, fmt = "json") {
    printGolden("```" + fmt);
    printGolden(msg);
    printGolden("```");
}

export function linebreak() {
    printGolden();
}
