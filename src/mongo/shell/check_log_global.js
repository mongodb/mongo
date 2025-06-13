// Populate global variables from modules for backwards compatibility

import {checkLog} from "src/mongo/shell/check_log.js";

globalThis.checkLog = checkLog;
