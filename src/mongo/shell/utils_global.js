// Populate global variables from modules for backwards compatibility

import {
    __magicNoPrint,
    __promptWrapper__,
    _awaitRSHostViaRSMonitor,
    _getErrorWithCode,
    _isSpiderMonkeyDebugEnabled,
    _shouldRetryWrites,
    _shouldUseImplicitSessions,
    _verboseShell,
    chatty,
    compare,
    compareOn,
    defaultPrompt,
    friendlyEqual,
    Geo,
    hasErrorCode,
    help,
    indentStr,
    isNetworkError,
    isRetryableError,
    jsTest,
    jsTestLog,
    jsTestName,
    jsTestOptions,
    printStackTrace,
    Random,
    retryOnNetworkError,
    retryOnRetryableError,
    rs,
    setVerboseShell,
    shellAutocomplete,
    shellHelper,
    shellPrintHelper,
    timestampCmp,
} from "src/mongo/shell/utils.js";

globalThis.__magicNoPrint = __magicNoPrint;
globalThis.__promptWrapper__ = __promptWrapper__;
globalThis._awaitRSHostViaRSMonitor = _awaitRSHostViaRSMonitor;
globalThis._getErrorWithCode = _getErrorWithCode;
globalThis._isSpiderMonkeyDebugEnabled = _isSpiderMonkeyDebugEnabled;
globalThis._shouldRetryWrites = _shouldRetryWrites;
globalThis._shouldUseImplicitSessions = _shouldUseImplicitSessions;
globalThis._verboseShell = _verboseShell;
globalThis.chatty = chatty;
globalThis.compare = compare;
globalThis.compareOn = compareOn;
globalThis.defaultPrompt = defaultPrompt;
globalThis.friendlyEqual = friendlyEqual;
globalThis.Geo = Geo;
globalThis.hasErrorCode = hasErrorCode;
globalThis.help = help;
globalThis.indentStr = indentStr;
globalThis.isNetworkError = isNetworkError;
globalThis.isRetryableError = isRetryableError;
globalThis.jsTest = jsTest;
globalThis.jsTestLog = jsTestLog;
globalThis.jsTestName = jsTestName;
globalThis.jsTestOptions = jsTestOptions;
globalThis.printStackTrace = printStackTrace;
globalThis.Random = Random;
globalThis.retryOnNetworkError = retryOnNetworkError;
globalThis.retryOnRetryableError = retryOnRetryableError;
globalThis.rs = rs;
globalThis.setVerboseShell = setVerboseShell;
globalThis.shellAutocomplete = shellAutocomplete;
globalThis.shellHelper = shellHelper;
globalThis.shellPrintHelper = shellPrintHelper;
globalThis.timestampCmp = timestampCmp;
