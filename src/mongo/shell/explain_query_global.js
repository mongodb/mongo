// Populate global variables from modules for backwards compatibility

import {DBExplainQuery} from "src/mongo/shell/explain_query.js";

globalThis.DBExplainQuery = DBExplainQuery;
