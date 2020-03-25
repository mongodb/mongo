// These commands were removed from mongos 4.6, but will still appear in the listCommands output
// of a 4.4 mongos. A last-stable mongos will be unable to run a command on a latest version shard
// that no longer supports that command. To increase test coverage and allow us to run on same- and
// mixed-version suites, we allow these commands to have a test defined without always existing on
// the servers being used.
const commandsRemovedFromMongosIn46 = [];
// These commands were added in mongos 4.6, so will not appear in the listCommands output of a
// 4.4 mongos. We will allow these commands to have a test defined without always existing on the
// mongos being used.
const commandsAddedToMongosIn46 = [];
