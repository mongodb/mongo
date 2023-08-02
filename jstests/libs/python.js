// Helper for finding the local python binary.

function getPython3Binary() {
    'use strict';

    // On windows it is important to use python vs python3
    // or else we will pick up a python that is not in our venv
    clearRawMongoProgramOutput();
    assert.eq(runNonMongoProgram("python", "--version"), 0);
    const pythonVersion = rawMongoProgramOutput();  // Will look like "Python 3.10.4\n"
    const usingPython39 = /Python 3\.9/.exec(pythonVersion);
    const usingPython310 = /Python 3\.10/.exec(pythonVersion);
    if (usingPython310) {
        print(
            "Found python 3.10 by default. Likely this is because we are using a virtual enviorment.");
        return "python";
    } else if (usingPython39) {
        // TODO: SERVER-79172
        // Once the above ticket is complete we should stop using python 3.9 on windows and upgrade
        // to python 310 everywhere To solve: grep for python39 and fix instances of it
        print(
            "Found python 3.9 by default. Likely this is because we are using a windows virtual enviorment.");
        return "python";
    }

    const paths = [
        "/opt/mongodbtoolchain/v4/bin/python3",
        "/cygdrive/c/python/python310/python.exe",
        "c:/python/python310/python.exe"
    ];
    for (let p of paths) {
        if (fileExists(p)) {
            print("Found python3 in default location " + p);
            return p;
        }
    }

    assert(/Python 3/.exec(pythonVersion));

    // We are probs running on mac
    print("Did not find python3 in a virtualenv or default location");
    return "python3";
}
