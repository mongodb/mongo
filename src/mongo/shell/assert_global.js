// Populate global variables from modules for backwards compatibility

import {assert, doassert, formatErrorMsg, sortDoc} from "src/mongo/shell/assert.js";

globalThis.assert = assert;
globalThis.doassert = doassert;
globalThis.sortDoc = sortDoc;
globalThis.formatErrorMsg = formatErrorMsg;
