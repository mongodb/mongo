// Test for invalid sasl auth mechanisms

function waitFailedToStart(pid, exitCode) {
    assert.soon(
        function () {
            const res = checkProgram(pid);
            if (res.alive) {
                return false;
            }

            return res.exitCode == exitCode;
        },
        `Failed to wait for ${pid} to die with exit code ${exitCode}`,
        60 * 1000,
    );
}

const m = MongoRunner.runMongod({
    setParameter: "authenticationMechanisms=SCRAM-SHA-1,foo",
    waitForConnect: false,
});

waitFailedToStart(m.pid, 2);
