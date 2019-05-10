// Test config file expansion using REST when permissions are too loose.
// @tags: [requires_http_client]

(function() {
    'use strict';

    load('jstests/noPassthrough/libs/configExpand/lib.js');

    if (_isWindows()) {
        print("Skipping test on windows");
        return;
    }

    const web = new ConfigExpandRestServer();
    web.start();

    const sicReflect = {
        setParameter: {scramIterationCount: {__rest: web.getStringReflectionURL('12345')}}
    };

    // Positive test just to be sure this works in a basic case before testing negatives.
    configExpandSuccess(sicReflect, null, {configExpand: 'rest', chmod: 0o600});

    // Still successful if writable by others, but not readable.
    configExpandSuccess(sicReflect, null, {configExpand: 'rest', chmod: 0o622});

    // Fail if readable by others.
    const expect = /is readable by non-owner users/;
    configExpandFailure(sicReflect, expect, {configExpand: 'rest', chmod: 0o666});
    configExpandFailure(sicReflect, expect, {configExpand: 'rest', chmod: 0o644});
    configExpandFailure(sicReflect, expect, {configExpand: 'rest', chmod: 0o660});
    configExpandFailure(sicReflect, expect, {configExpand: 'rest', chmod: 0o606});

    web.stop();
})();
