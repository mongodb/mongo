/**
 * Helpers for dropping collections or databases created by a workload
 * during its execution.
 */

export function dropCollections(db, pattern) {
    assert(pattern instanceof RegExp, "expected pattern to be a regular expression");

    db.getCollectionInfos()
        .filter(function (collInfo) {
            return pattern.test(collInfo.name);
        })
        .forEach(function (collInfo) {
            assert(db[collInfo.name].drop());
        });
}

export function dropDatabases(db, pattern) {
    assert(pattern instanceof RegExp, "expected pattern to be a regular expression");

    let res = db.adminCommand("listDatabases");
    assert.commandWorked(res);

    res.databases.forEach(function (dbInfo) {
        if (pattern.test(dbInfo.name)) {
            let res = db.getSiblingDB(dbInfo.name).dropDatabase();
            assert.commandWorked(res);
        }
    });
}

/**
 * Helper for dropping roles or users that were created by a workload
 * during its execution.
 */
export function dropUtilRetry(elems, cb, message) {
    const kNumRetries = 5;
    const kRetryInterval = 5000;

    assert.retry(
        function () {
            elems = elems.filter((elem) => !cb(elem));
            return elems.length === 0;
        },
        message,
        kNumRetries,
        kRetryInterval,
    );
}

export function dropRoles(db, pattern) {
    assert(pattern instanceof RegExp, "expected pattern to be a regular expression");
    const rolesToDrop = db
        .getRoles()
        .map((ri) => ri.role)
        .filter((r) => pattern.test(r));

    dropUtilRetry(
        rolesToDrop,
        (role) => db.dropRole(role),
        "Failed dropping roles: " + tojson(rolesToDrop) + " from database " + db.getName(),
    );
}

export function dropUsers(db, pattern) {
    assert(pattern instanceof RegExp, "expected pattern to be a regular expression");
    const usersToDrop = db
        .getUsers()
        .map((ui) => ui.user)
        .filter((u) => pattern.test(u));

    dropUtilRetry(
        usersToDrop,
        (user) => db.dropUser(user),
        "Failed dropping users: " + tojson(usersToDrop) + " from database " + db.getName(),
    );
}
