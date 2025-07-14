// Populate global variables from modules for backwards compatibility

import {
    MongoRunner,
    myPort,
    runMongoProgram,
    startMongoProgram,
    startMongoProgramNoConnect
} from "src/mongo/shell/servers.js";

globalThis.MongoRunner = MongoRunner;
globalThis.myPort = myPort;
globalThis.runMongoProgram = runMongoProgram;
globalThis.startMongoProgram = startMongoProgram;
globalThis.startMongoProgramNoConnect = startMongoProgramNoConnect;
