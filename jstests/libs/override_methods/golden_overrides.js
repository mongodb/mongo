// Override print to output to both stdout and the golden file.
// This affects everything that uses print: printjson, jsTestLog, etc.
globalThis.print = (() => {
    const original = globalThis.print;
    return function print(...args) {
        // Imitate GlobalInfo::Functions::print::call.
        let str = args.map(a => a == null ? '[unknown type]' : a).join(' ');

        // Make sure each print() call ends in a newline.
        //
        // From manual testing, it seems (print('a'), print('b')) behaves the same as
        // (print('a\n'), print('b\n')); that behavior must be to ensure each print call appears on
        // its own line for readability. In the context of golden testing, we want to match that
        // behavior, and this also ensures the test output is a proper text file
        // (newline-terminated).
        if (str.slice(-1) !== '\n') {
            str += '\n';
        }

        _writeGoldenData(str);

        return original(...args);
    };
})();

// Initialize `printGolden` to have the same behavior as `print`. This is needed to utilize markdown
// support (i.e. pretty_md.js) in this golden test suite.
globalThis.printGolden = globalThis.print;
