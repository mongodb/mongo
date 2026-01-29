// Populate global variables from modules for backwards compatibility

import {
    MongoRunner,
    runMongoProgram,
    startMongoProgram,
    startMongoProgramNoConnect,
    getX509Path,
} from "src/mongo/shell/servers.js";

globalThis.MongoRunner = MongoRunner;
globalThis.runMongoProgram = runMongoProgram;
globalThis.startMongoProgram = startMongoProgram;
globalThis.startMongoProgramNoConnect = startMongoProgramNoConnect;
globalThis.getX509Path = getX509Path;
