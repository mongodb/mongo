// These commands were removed from mongos since the last LTS version, but will still appear in the
// listCommands output of a last LTS version mongos. A last-stable mongos will be unable to
// run a command on a latest version shard that no longer supports that command. To increase test
// coverage and allow us to run on same- and mixed-version suites, we allow these commands to have a
// test defined without always existing on the servers being used.
const commandsRemovedFromMongosSinceLastLTS = [];
// These commands were added in mongos since the last LTS version, so will not appear in the
// listCommands output of a last LTS version mongos. We will allow these commands to have a test
// defined without always existing on the mongos being used.
const commandsAddedToMongosSinceLastLTS = ["reshardCollection"];
