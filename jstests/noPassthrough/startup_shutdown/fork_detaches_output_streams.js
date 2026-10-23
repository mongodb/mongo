/**
 * Tests for the --forkDetachesOutputStreams server option.
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {isLinux} from "jstests/libs/server_security/os_helpers.js";
import {
    forkThenShutdownMongod,
    forkThenShutdownMongos,
    startThenShutdownMongod,
    startThenShutdownMongos,
} from "jstests/noPassthrough/libs/startup_shutdown_helpers.js";

if (!isLinux()) {
    jsTestLog("Skipping test due to non-linux platform.");
    quit();
}

// Markers that will be written directly to standard streams by a failpoint.
const markers = {
    stdout: "stdout marker",
    stderr: "stderr marker",
};

// setParameter options to enable the failpoints we'll use to write to standard streams
// and subsequently shutdown cleanly.
const failpointParameters = {
    "failpoint.writeToStandardStreamsAtStartup": tojsononeline({mode: "alwaysOn", data: markers}),
    "failpoint.shutdownAtStartup": tojsononeline({mode: "alwaysOn"}),
};

/**
 * Returns whether or not the given `text` contains a line that is exactly `marker`,
 * accounting for the shell's prefixing of lines to indicate which process they came
 * from e.g., "d20040| <marker>" for mongod and "s20040| <marker>" for mongos.
 */
function containsMarker(text, marker) {
    return text.split("\n").some((line) => line.replace(/^[ds]\d+\| /, "") === marker);
}

/**
 * Starts a forked mongod with the given command line options plus startup failpoints that cause
 * writes to stdout and stderr and subsequently make the forked daemon shutdown. The failpoint
 * actions all happen post-forking. Returns the raw output (which is unfortunately merged into
 * a single stream).
 */
function runMongodWithStartupFailpoints(args) {
    return forkThenShutdownMongod({...args, setParameter: failpointParameters});
}

/**
 * Asserts that the marker lines appear in the raw program output only if the
 * forkDetachesOutputStreams option is false.
 */
function assertMarkerLines(args, rawOutput) {
    const keepStreams = args.forkDetachesOutputStreams === false;
    for (const stream of ["stdout", "stderr"]) {
        const markerLine = containsMarker(rawOutput, markers[stream]);
        if (keepStreams) {
            assert(markerLine, `expected a ${stream} marker line in program output`, {rawOutput});
        } else {
            assert(!markerLine, `expected no ${stream} marker line in program output`, {rawOutput});
        }
    }
}

function testMongod(args) {
    const rawOutput = runMongodWithStartupFailpoints(args);
    assertMarkerLines(args, rawOutput);
}

/**
 * Starts a forked mongos with the given command line options plus startup failpoints that cause
 * writes to stdout and stderr and subsequently make the forked daemon shutdown. The failpoint
 * actions all happen post-forking. Returns the raw output (which is unfortunately merged into a
 * single stream).
 */
function runMongosWithStartupFailpoints(configRS, args) {
    return forkThenShutdownMongos(configRS, {...args, setParameter: failpointParameters});
}

function testMongos(configRS, args) {
    const rawOutput = runMongosWithStartupFailpoints(configRS, args);
    assertMarkerLines(args, rawOutput);
}

describe("mongod forkDetachesOutputStreams", function () {
    // Detaching is the default.
    it("detaches output streams by default", function () {
        testMongod({});
    });
    it("handles forkDetachesOutputStreams=true", function () {
        testMongod({forkDetachesOutputStreams: true});
    });
    it("handles forkDetachesOutputStreams=false", function () {
        testMongod({forkDetachesOutputStreams: false});
    });
    it("handles --forkDetachesOutputStreams as a flag", function () {
        testMongod({forkDetachesOutputStreams: ""});
    });

    // Without --fork, the option is accepted but has no effect.
    it("accepts forkDetachesOutputStreams=false without --fork", function () {
        startThenShutdownMongod({forkDetachesOutputStreams: false});
    });
    it("accepts forkDetachesOutputStreams=true without --fork", function () {
        startThenShutdownMongod({forkDetachesOutputStreams: true});
    });
});

describe("mongos forkDetachesOutputStreams", function () {
    // For mongos to start up successfully, it needs a config server to connect to.
    let configRS;

    before(function () {
        configRS = new ReplSetTest({nodes: 1});
        configRS.startSet({configsvr: ""});
        configRS.initiate();
    });
    after(function () {
        configRS.stopSet();
    });

    it("detaches output streams by default", function () {
        testMongos(configRS, {});
    });
    it("handles forkDetachesOutputStreams=true", function () {
        testMongos(configRS, {forkDetachesOutputStreams: true});
    });
    it("handles forkDetachesOutputStreams=false", function () {
        testMongos(configRS, {forkDetachesOutputStreams: false});
    });
    it("handles --forkDetachesOutputStreams as a flag", function () {
        testMongos(configRS, {forkDetachesOutputStreams: ""});
    });

    // Without --fork, the option is accepted but has no effect.
    it("accepts forkDetachesOutputStreams=false without --fork", function () {
        startThenShutdownMongos(configRS, {forkDetachesOutputStreams: false});
    });
    it("accepts forkDetachesOutputStreams=true without --fork", function () {
        startThenShutdownMongos(configRS, {forkDetachesOutputStreams: true});
    });
});
