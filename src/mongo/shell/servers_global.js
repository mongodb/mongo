// Populate global variables from modules for backwards compatibility

import {
    MongoRunner,
    runMongoProgram,
    startMongoProgram,
    startMongoProgramNoConnect
} from "src/mongo/shell/servers.js";

globalThis.MongoRunner = MongoRunner;
globalThis.runMongoProgram = runMongoProgram;
globalThis.startMongoProgram = startMongoProgram;
globalThis.startMongoProgramNoConnect = startMongoProgramNoConnect;
