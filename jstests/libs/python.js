// Helper for finding the local python binary.

export function getPython3Binary() {
    // On windows it is important to use python vs python3
    // or else we will pick up a python that is not in our venv
    clearRawMongoProgramOutput();
    assert.eq(runNonMongoProgram("python", "--version"), 0);
    const pythonVersion = rawMongoProgramOutput("Python"); // Will look like "Python 3.13.4\n"
    const usingPython313 = /Python 3\.13/.exec(pythonVersion);
    if (usingPython313) {
        jsTest.log.info("Found python 3.13 by default. Likely this is because we are using a virtual environment.");
        return "python";
    }

    const paths = [
        "/opt/mongodbtoolchain/v5/bin/python3.13",
        "/opt/mongodbtoolchain/v4/bin/python3",
        "/cygdrive/c/python/python313/python.exe",
        "c:/python/python313/python.exe",
    ];
    for (let p of paths) {
        if (fileExists(p)) {
            jsTest.log.info("Found python3 in default location " + p);
            return p;
        }
    }

    assert(/Python 3/.exec(pythonVersion));

    // We are probs running on mac
    jsTest.log.info("Did not find python3 in a virtualenv or default location");
    return "python3";
}
