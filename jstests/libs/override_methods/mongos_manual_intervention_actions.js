/**
 * If the config primary steps down during a metadata command, mongos will internally retry the
 * command. On the retry, the command may fail with the error "ManualInterventionRequired" if
 * the earlier try left the config database in an inconsistent state.
 *
 * This override allows for automating the manual cleanup by catching the
 * "ManualInterventionRequired" error, performing the cleanup, and transparently retrying the
 * command.
 */
(function() {
    let manualInterventionActions = {
        removePartiallyWrittenChunks: function(mongosConn, ns, cmdObj, numAttempts) {
            print("command " + tojson(cmdObj) + " failed after " + numAttempts +
                  " attempts due to seeing partially written chunks for collection " + ns +
                  ", probably due to a previous failed shardCollection attempt. Manually" +
                  " deleting chunks for " + ns + " from config.chunks and retrying the command.");
            assert.writeOK(mongosConn.getDB("config").chunks.remove(
                {ns: ns}, {writeConcern: {w: "majority"}}));
        }
    };

    const mongoRunCommandOriginal = Mongo.prototype.runCommand;

    Mongo.prototype.runCommand = function runCommand(dbName, cmdObj, options) {
        const cmdName = Object.keys(cmdObj)[0];
        const commandsToRetry =
            new Set(["mapReduce", "mapreduce", "shardCollection", "shardcollection"]);

        if (!commandsToRetry.has(cmdName)) {
            return mongoRunCommandOriginal.apply(this, arguments);
        }

        const maxAttempts = 10;
        let numAttempts = 0;
        let res;

        while (numAttempts < maxAttempts) {
            res = mongoRunCommandOriginal.apply(this, arguments);
            ++numAttempts;

            if (res.ok === 1 || res.code !== ErrorCodes.ManualInterventionRequired ||
                numAttempts === maxAttempts) {
                break;
            }

            if (cmdName === "shardCollection" || cmdName === "shardcollection") {
                const ns = cmdObj[cmdName];
                manualInterventionActions.removePartiallyWrittenChunks(
                    this, ns, cmdObj, numAttempts);
            } else if (cmdName === "mapReduce" || cmdName === "mapreduce") {
                const out = cmdObj.out;

                // The output collection can be specified as a string argument to the mapReduce
                // command's 'out' option, or nested under 'out.replace', 'out.merge', or
                // 'out.reduce'.
                let outCollName;
                if (typeof out === "string") {
                    outCollName = out;
                } else if (typeof out === "object") {
                    outCollName = out.replace || out.merge || out.reduce;
                } else {
                    print("Could not parse the output collection's name from 'out' option in " +
                          tojson(cmdObj) + "; not retrying on ManualInterventionRequired error " +
                          tojson(res));
                    break;
                }

                // The output collection's database can optionally be specified under 'out.db',
                // else it defaults to the input collection's database.
                const outDbName = out.db || dbName;

                const ns = outDbName + "." + outCollName;
                manualInterventionActions.removePartiallyWrittenChunks(
                    this, ns, cmdObj, numAttempts);
            }
        }
        return res;
    };
})();
