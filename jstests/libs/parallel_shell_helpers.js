/**
 * Allows passing arguments to the function executed by startParallelShell.
 */
const funWithArgs = (fn, ...args) =>
    "(" + fn.toString() + ")(" + args.map(x => tojson(x)).reduce((x, y) => x + ", " + y) + ")";
