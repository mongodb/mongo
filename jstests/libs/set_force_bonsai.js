/**
 * Set internalQueryFrameworkControl to forceBonsai. Intended to be used by tasks which must force
 * bonsai regardless of the configuration of the variant running the task, since the suite
 * definition cannot override a knob which is also defined by the variant.
 */
(function() {
'use strict';

assert.commandWorked(
    db.adminCommand({setParameter: 1, internalQueryFrameworkControl: "forceBonsai"}));
})();
