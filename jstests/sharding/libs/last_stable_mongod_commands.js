// These commands were removed from mongod in 4.6, but will still appear in the listCommands output
// of a 4.4 mongod. To increase test coverage and allow us to run on same- and
// mixed-version suites, we allow these commands to have a test defined without always existing on
// the servers being used.
const commandsRemovedFromMongodIn46 = ["_configsvrCreateCollection", "mapreduce.shardedfinish"];
// These commands were added in mongod 4.6, so will not appear in the listCommands output of a
// 4.4 mongod. We will allow these commands to have a test defined without always existing on the
// mongod being used.
const commandsAddedToMongodIn46 = [];
