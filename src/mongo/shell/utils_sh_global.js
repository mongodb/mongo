// Populate global variables from modules for backwards compatibility

import {
    printShardingStatus,
    sh,
} from "src/mongo/shell/utils_sh.js";

globalThis.printShardingStatus = printShardingStatus;
globalThis.sh = sh;
