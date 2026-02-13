/**
 * Provides helper functions to output content to markdown. This is used for golden testing, using
 * `print` to write to the expected output files.
 */

let sectionCount = 1;
export function section(msg) {
    print(`## ${sectionCount}.`, msg + '\n');
    sectionCount++;
}

export function subSection(msg) {
    print("###", msg + '\n');
}

export function code(msg, fmt = "json") {
    print("```" + fmt + "\n");
    print(msg);
    if (msg.slice(-1) !== '\n') {
        print('\n');
    }
    print("```\n");
}

export function linebreak() {
    print('\n');
}
