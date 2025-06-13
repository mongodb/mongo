// Populate global variables from modules for backwards compatibility

import {DB} from "src/mongo/shell/db.js";

globalThis.DB = DB;
