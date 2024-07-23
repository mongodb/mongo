// TODO SERVER-92693: Use only one overrides file (golden_overrides.js) for golden tests.

// Initialize printGolden to output to both stdout and the golden file.
// Note that no other print functions are overriden here, so those won't output to the golden file.
globalThis.printGolden = function(...args) {
    let str = args.map(a => a == null ? '[unknown type]' : a).join(' ');

    // Make sure each printGolden() call ends in a newline.
    if (str.slice(-1) !== '\n') {
        str += '\n';
    }

    _writeGoldenData(str);
    print(...args);
};
