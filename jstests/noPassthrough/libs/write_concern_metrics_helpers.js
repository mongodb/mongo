// Helper functions for tests verifying server status writeConcern metrics

export function verifyServerStatusFields(serverStatusResponse) {
    assert(serverStatusResponse.hasOwnProperty("opWriteConcernCounters"),
           "Expected the serverStatus response to have a 'opWriteConcernCounters' field\n" +
               tojson(serverStatusResponse));
    assert(serverStatusResponse.opWriteConcernCounters.hasOwnProperty("insert"),
           "The 'opWriteConcernCounters' field in serverStatus did not have the 'insert' field\n" +
               tojson(serverStatusResponse.opWriteConcernCounters));
    assert(serverStatusResponse.opWriteConcernCounters.hasOwnProperty("update"),
           "The 'opWriteConcernCounters' field in serverStatus did not have the 'update' field\n" +
               tojson(serverStatusResponse.opWriteConcernCounters));
    assert(serverStatusResponse.opWriteConcernCounters.hasOwnProperty("delete"),
           "The 'opWriteConcernCounters' field in serverStatus did not have the 'delete' field\n" +
               tojson(serverStatusResponse.opWriteConcernCounters));
}

// Verifies that the given path of the server status response is incremented in the way we
// expect, and no other changes occurred. This function modifies its inputs.
export function verifyServerStatusChange(initialStats, newStats, paths, expectedIncrement) {
    paths.forEach(path => {
        // Traverse to the parent of the changed element.
        let pathComponents = path.split(".");
        let initialParent = initialStats;
        let newParent = newStats;
        for (let i = 0; i < pathComponents.length - 1; i++) {
            assert(initialParent.hasOwnProperty(pathComponents[i]),
                   "initialStats did not contain component " + i + " of path " + path +
                       ", initialStats: " + tojson(initialStats));
            initialParent = initialParent[pathComponents[i]];

            assert(newParent.hasOwnProperty(pathComponents[i]),
                   "newStats did not contain component " + i + " of path " + path +
                       ", newStats: " + tojson(newStats));
            newParent = newParent[pathComponents[i]];
        }

        // Test the expected increment of the changed element. The element may not exist in the
        // initial stats, in which case it is treated as 0.
        let lastPathComponent = pathComponents[pathComponents.length - 1];
        let initialValue = 0;
        if (initialParent.hasOwnProperty(lastPathComponent)) {
            initialValue = initialParent[lastPathComponent];
        }
        assert(newParent.hasOwnProperty(lastPathComponent),
               "newStats did not contain last component of path " + path +
                   ", newStats: " + tojson(newStats));
        assert.eq(initialValue + expectedIncrement,
                  newParent[lastPathComponent],
                  "expected " + path + " to increase by " + expectedIncrement + ", initialStats: " +
                      tojson(initialStats) + ", newStats: " + tojson(newStats));

        // Delete the changed element.
        delete initialParent[lastPathComponent];
        delete newParent[lastPathComponent];
    });

    // The stats objects should be equal without the changed element.
    assert.eq(0,
              bsonWoCompare(initialStats, newStats),
              "expected initialStats and newStats to be equal after removing " + tojson(paths) +
                  ", initialStats: " + tojson(initialStats) + ", newStats: " + tojson(newStats));
}

// Generate commands that will be using default write concern.
export function generateCmdsWithNoWCProvided(cmd) {
    return [
        cmd,
        // Missing 'w' field will be filled with default write concern.
        Object.assign(Object.assign({}, cmd), {writeConcern: {}}),
        Object.assign(Object.assign({}, cmd), {writeConcern: {j: true}}),
        Object.assign(Object.assign({}, cmd), {writeConcern: {wtimeout: 15000}})
    ];
}
