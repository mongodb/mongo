// Test config file expansion using REST at top level.
// @tags: [requires_http_client]

(function() {
'use strict';

load('jstests/noPassthrough/libs/configExpand/lib.js');

const web = new ConfigExpandRestServer();
web.start();

// Unexpected elements.
configExpandFailure({
    setParameter: {
        scramIterationCount: {__rest: web.getStringReflectionURL('12345'), foo: 'bar'},
    }
},
                    /expansion block must contain only '__rest'/);

const sicReflect = {
    setParameter: {scramIterationCount: {__rest: web.getStringReflectionURL('12345')}}
};

// Positive test just to be sure this works in a basic case before testing negatives.
configExpandSuccess(sicReflect);

// Expansion not enabled.
configExpandFailure(sicReflect, /__rest support has not been enabled/, {configExpand: 'none'});

// Expansion enabled, but not recursively.
configExpandFailure(
    {__rest: web.getURL() + '/reflect/yaml?yaml=' + encodeURI(jsToYaml(sicReflect)), type: 'yaml'},
    /__rest support has not been enabled/);

web.stop();
})();
