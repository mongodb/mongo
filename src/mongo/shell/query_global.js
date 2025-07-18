// Populate global variables from modules for backwards compatibility

import {DBCommandCursor, DBQuery, QueryHelpers} from "src/mongo/shell/query.js";

globalThis.DBCommandCursor = DBCommandCursor;
globalThis.DBQuery = DBQuery;
globalThis.QueryHelpers = QueryHelpers;
