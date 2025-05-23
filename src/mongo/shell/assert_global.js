// Populate global variables from modules for backwards compatibility

import {
    _convertExceptionToReturnStatus,
    assert,
    doassert,
    formatErrorMsg,
    sortDoc
} from "src/mongo/shell/assert.js";

globalThis._convertExceptionToReturnStatus = _convertExceptionToReturnStatus;
globalThis.assert = assert;
globalThis.doassert = doassert;
globalThis.sortDoc = sortDoc;
globalThis.formatErrorMsg = formatErrorMsg;
