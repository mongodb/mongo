/**
 * Tests the default values for causal consistency and retryable writes as part of SessionOptions.
 */
const conn = MongoRunner.runMongod();

let session = conn.startSession();
assert(
    session.getOptions().isCausalConsistency(),
    "Causal consistency should be implicitly enabled for an explicit session",
);
assert(
    !session.getOptions().shouldRetryWrites(),
    "Retryable writes should not be implicitly enabled for an explicit session",
);
session.endSession();

session = conn.startSession({causalConsistency: true});
assert(session.getOptions().isCausalConsistency(), "Causal consistency should be able to be explicitly enabled");
assert(
    !session.getOptions().shouldRetryWrites(),
    "Retryable writes should not be implicitly enabled for an explicit session",
);
session.endSession();

session = conn.startSession({causalConsistency: false});
assert(!session.getOptions().isCausalConsistency(), "Causal consistency should be able to be explicitly disabled");
assert(
    !session.getOptions().shouldRetryWrites(),
    "Retryable writes should not be implicitly enabled for an explicit session",
);
session.endSession();

session = conn.startSession({retryWrites: false});
assert(
    session.getOptions().isCausalConsistency(),
    "Causal consistency should be implicitly enabled for an explicit session",
);
assert(!session.getOptions().shouldRetryWrites(), "Retryable writes should be able to be explicitly disabled");
session.endSession();

session = conn.startSession({retryWrites: true});
assert(
    session.getOptions().isCausalConsistency(),
    "Causal consistency should be implicitly enabled for an explicit session",
);
assert(session.getOptions().shouldRetryWrites(), "Retryable writes should be able to be explicitly enabled");
session.endSession();

function runMongoShellWithRetryWritesEnabled(func) {
    const args = [MongoRunner.getMongoShellPath()];
    args.push("--port", conn.port);
    args.push("--retryWrites");

    const jsCode = "(" + func.toString() + ")()";
    args.push("--eval", jsCode);

    const exitCode = runMongoProgram(...args);
    assert.eq(0, exitCode, "Encountered an error in the other mongo shell");
}

runMongoShellWithRetryWritesEnabled(function () {
    let session = db.getSession();
    assert(
        session.getOptions().isCausalConsistency(),
        "Causal consistency should be implicitly enabled for an explicit session",
    );
    assert(
        session.getOptions().shouldRetryWrites(),
        "Retryable writes should be implicitly enabled on default session when using" + " --retryWrites",
    );

    session = db.getMongo().startSession({retryWrites: false});
    assert(
        session.getOptions().isCausalConsistency(),
        "Causal consistency should be implicitly enabled for an explicit session",
    );
    assert(!session.getOptions().shouldRetryWrites(), "Retryable writes should be able to be explicitly disabled");
    session.endSession();

    session = db.getMongo().startSession();
    assert(
        session.getOptions().isCausalConsistency(),
        "Causal consistency should be implicitly enabled for an explicit session",
    );
    assert(
        session.getOptions().shouldRetryWrites(),
        "Retryable writes should be implicitly enabled on new sessions when using" + " --retryWrites",
    );
    session.endSession();
});

MongoRunner.stopMongod(conn);
