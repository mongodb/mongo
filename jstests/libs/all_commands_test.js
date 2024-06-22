/**
 * A library for testing behaviors over the set of all available commands.
 * Users of this library must declare the expected results for all commands and pass
 * them into a cmdMap object.
 *
 * Each entry in the map should have at least the following fields:
 *      {
 *          command: {<command object, e.g. 'find: test, filter: {a: 1}'>}
 *          skip: <reason string> // optional field
 *      }
 *
 * All the logic about how exactly a test should run is defined by the user.
 * See the 'testAllCommands' function.
 */
export const AllCommandsTest = (function() {
    /**
     * Verifies that the command map contains an entry for every command that exists on the server.
     * This is already called in 'testAllCommands', so there is no need to call this directly.
     *
     * @param {Object} conn The shell connection to run the suite over.
     * @param {map} cmdMap A map of all commands, with their invocations and expected behavior.
     * @param {function} skipfn A user-defined optional function is run to decide whether the
     *     specific command should be skipped.
     */
    function checkCommandCoverage(conn, cmdMap, skipfn) {
        const res = assert.commandWorked(conn.adminCommand({listCommands: 1}));
        const commandsInListCommands = Object.keys(res.commands);
        let missingCommands = [];
        let includedCommands = [];

        // Make sure that all valid commands are covered in the cmdMap.
        for (const command of commandsInListCommands) {
            if (skipfn !== undefined) {
                if (skipfn(command)) {
                    continue;
                }
            }
            if (!cmdMap[command]) {
                missingCommands.push(command);
            } else {
                includedCommands.push(command);
            }
        }
        if (missingCommands.length !== 0) {
            throw new Error("Command map is missing entries for " + missingCommands);
        }

        return includedCommands;
    }

    /**
     * The runner function for this library.
     * Use the 'skip' option for tests that should not run.
     *
     * @param {Object} conn The shell connection to run the suite over.
     * @param {map} cmdMap A map of all commands, with their invocations and expected behavior.
     * @param {function} testFn A user-defined function to execute on every command.
     */
    function testAllCommands(conn, cmdMap, testFn) {
        // First check that the map contains all available commands.
        const commands = checkCommandCoverage(conn, cmdMap);

        for (const command of commands) {
            const test = cmdMap[command];

            // Coverage already guaranteed above, but check again just in case.
            assert(test, "Coverage failure: must explicitly define a test for " + command);

            if (test.skip !== undefined) {
                jsTestLog("Skipping " + command + ": " + test.skip);
                continue;
            }

            // Run logic specified by caller.
            jsTestLog("Testing " + command);
            testFn(test, conn);
        }
    }

    return {testAllCommands: testAllCommands, checkCommandCoverage: checkCommandCoverage};
})();
