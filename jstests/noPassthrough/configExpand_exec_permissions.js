// Test config file expansion using EXEC when permissions are too loose.
// Ideally, we'd also check for foreign ownership here,
// but that's impractical in a test suite where we're not running as root.

(function() {
'use strict';

if (_isWindows()) {
    print("Skipping test on windows");
    return;
}

load('jstests/noPassthrough/libs/configExpand/lib.js');

const sicReflect = {
    setParameter: {scramIterationCount: {__exec: makeReflectionCmd('12345')}}
};

// Positive test just to be sure this works in a basic case before testing negatives.
configExpandSuccess(sicReflect, null, {configExpand: 'exec', chmod: 0o600});

// Still successful if readable by others, but not writable.
configExpandSuccess(sicReflect, null, {configExpand: 'exec', chmod: 0o644});

// Fail if writable by others.
const expect = /is writable by non-owner users/;
configExpandFailure(sicReflect, expect, {configExpand: 'exec', chmod: 0o666});
configExpandFailure(sicReflect, expect, {configExpand: 'exec', chmod: 0o622});
configExpandFailure(sicReflect, expect, {configExpand: 'exec', chmod: 0o660});
configExpandFailure(sicReflect, expect, {configExpand: 'exec', chmod: 0o606});

// Explicitly world-readable/writable config file without expansions should be fine.
configExpandSuccess({}, null, {configExpand: 'none', chmod: 0o666});
})();
