// Populate global variables from modules for backwards compatibility

import {
    __magicNoPrint,
    __prompt__,
    __promptWrapper__,
    _awaitRSHostViaRSMonitor,
    _barFormat,
    _getErrorWithCode,
    _isSpiderMonkeyDebugEnabled,
    _originalPrint,
    _shouldRetryWrites,
    _shouldUseImplicitSessions,
    _validateMemberIndex,
    _verboseShell,
    chatty,
    compare,
    compareOn,
    defaultPrompt,
    disablePrint,
    enablePrint,
    executeNoThrowNetworkError,
    friendlyEqual,
    Geo,
    hasErrorCode,
    helloStatePrompt,
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
    replSetMemberStatePrompt,
    retryOnNetworkError,
    retryOnRetryableError,
    rs,
    setVerboseShell,
    shellAutocomplete,
    shellHelper,
    shellPrint,
    shellPrintHelper,
    timestampCmp,
} from "src/mongo/shell/utils.js";

globalThis.__magicNoPrint = __magicNoPrint;
globalThis.__prompt__ = __prompt__;
globalThis.__promptWrapper__ = __promptWrapper__;
globalThis._awaitRSHostViaRSMonitor = _awaitRSHostViaRSMonitor;
globalThis._barFormat = _barFormat;
globalThis._getErrorWithCode = _getErrorWithCode;
globalThis._isSpiderMonkeyDebugEnabled = _isSpiderMonkeyDebugEnabled;
globalThis._originalPrint = _originalPrint;
globalThis._shouldRetryWrites = _shouldRetryWrites;
globalThis._shouldUseImplicitSessions = _shouldUseImplicitSessions;
globalThis._validateMemberIndex = _validateMemberIndex;
globalThis._verboseShell = _verboseShell;
globalThis.chatty = chatty;
globalThis.compare = compare;
globalThis.compareOn = compareOn;
globalThis.defaultPrompt = defaultPrompt;
globalThis.disablePrint = disablePrint;
globalThis.enablePrint = enablePrint;
globalThis.executeNoThrowNetworkError = executeNoThrowNetworkError;
globalThis.friendlyEqual = friendlyEqual;
globalThis.Geo = Geo;
globalThis.hasErrorCode = hasErrorCode;
globalThis.helloStatePrompt = helloStatePrompt;
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
globalThis.replSetMemberStatePrompt = replSetMemberStatePrompt;
globalThis.retryOnNetworkError = retryOnNetworkError;
globalThis.retryOnRetryableError = retryOnRetryableError;
globalThis.rs = rs;
globalThis.setVerboseShell = setVerboseShell;
globalThis.shellAutocomplete = shellAutocomplete;
globalThis.shellHelper = shellHelper;
globalThis.shellPrint = shellPrint;
globalThis.shellPrintHelper = shellPrintHelper;
globalThis.timestampCmp = timestampCmp;
