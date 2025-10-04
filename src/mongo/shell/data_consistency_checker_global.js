// Populate global variables from modules for backwards compatibility

import {CollInfos, DataConsistencyChecker} from "src/mongo/shell/data_consistency_checker.js";

globalThis.CollInfos = CollInfos;
globalThis.DataConsistencyChecker = DataConsistencyChecker;
