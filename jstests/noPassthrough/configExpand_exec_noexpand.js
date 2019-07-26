// Test config file expansion using EXEC.

(function() {
'use strict';

load('jstests/noPassthrough/libs/configExpand/lib.js');

// Unexpected elements.
configExpandFailure({
    setParameter: {
        scramIterationCount: {__exec: makeReflectionCmd('12345'), foo: 'bar'},
    }
},
                    /expansion block must contain only '__exec'/);

const sicReflect = {
    setParameter: {scramIterationCount: {__exec: makeReflectionCmd('12345')}}
};

// Positive test just to be sure this works in a basic case before testing negatives.
configExpandSuccess(sicReflect);

// Expansion not enabled.
configExpandFailure(sicReflect, /__exec support has not been enabled/, {configExpand: 'none'});

// Expansion enabled, but not recursively.
configExpandFailure({__exec: makeReflectionCmd(jsToYaml(sicReflect)), type: 'yaml'},
                    /__exec support has not been enabled/);
})();
