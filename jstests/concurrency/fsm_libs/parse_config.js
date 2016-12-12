'use strict';

// Validate the config object and return a normalized copy of it.
// Normalized means all optional parameters are set to their default values,
// and any parameters that need to be coerced have been coerced.
function parseConfig(config) {
    config = Object.extend({}, config, true);  // defensive deep copy

    var allowedKeys = [
        'data',
        'iterations',
        'passConnectionCache',
        'setup',
        'skip',
        'startState',
        'states',
        'teardown',
        'threadCount',
        'transitions'
    ];

    Object.keys(config).forEach(function(key) {
        assert.contains(
            key,
            allowedKeys,
            'invalid config parameter: ' + key + '; valid parameters are: ' + tojson(allowedKeys));
    });

    assert(Number.isInteger(config.threadCount), 'expected number of threads to be an integer');
    assert.gt(config.threadCount, 0, 'expected number of threads to be positive');

    assert(Number.isInteger(config.iterations), 'expected number of iterations to be an integer');
    assert.gt(config.iterations, 0, 'expected number of iterations to be positive');

    config.startState = config.startState || 'init';
    assert.eq('string', typeof config.startState);

    assert.eq('object', typeof config.states);
    assert.gt(Object.keys(config.states).length, 0);
    Object.keys(config.states).forEach(function(k) {
        assert.eq('function', typeof config.states[k], 'config.states.' + k + ' is not a function');
        if (config.passConnectionCache) {
            assert.eq(3,
                      config.states[k].length,
                      'if passConnectionCache is true, state functions should ' +
                          'accept 3 parameters: db, collName, and connCache');
        } else {
            assert.eq(2,
                      config.states[k].length,
                      'if passConnectionCache is false, state functions should ' +
                          'accept 2 parameters: db and collName');
        }
    });

    // assert all states mentioned in config.transitions are present in config.states
    assert.eq('object', typeof config.transitions);
    assert.gt(Object.keys(config.transitions).length, 0);
    Object.keys(config.transitions).forEach(function(fromState) {
        assert(config.states.hasOwnProperty(fromState),
               'config.transitions contains a state not in config.states: ' + fromState);

        assert.gt(Object.keys(config.transitions[fromState]).length, 0);
        Object.keys(config.transitions[fromState]).forEach(function(toState) {
            assert(config.states.hasOwnProperty(toState),
                   'config.transitions.' + fromState + ' contains a state not in config.states: ' +
                       toState);
            assert.eq('number',
                      typeof config.transitions[fromState][toState],
                      'transitions.' + fromState + '.' + toState + ' should be a number');
            assert(!isNaN(config.transitions[fromState][toState]),
                   'transitions.' + fromState + '.' + toState + ' cannot be NaN');
        });
    });

    config.setup = config.setup || function() {};
    assert.eq('function', typeof config.setup);

    config.skip = config.skip || function() {
        return {skip: false};
    };
    assert.eq('function', typeof config.skip);

    config.teardown = config.teardown || function() {};
    assert.eq('function', typeof config.teardown);

    config.data = config.data || {};
    assert.eq('object', typeof config.data);
    assert.eq(false, config.data.hasOwnProperty('tid'), 'data object cannot redefine "tid"');
    assert.eq(false,
              config.data.hasOwnProperty('iterations'),
              'data object cannot redefine "iterations"');
    assert.eq(false,
              config.data.hasOwnProperty('threadCount'),
              'data object cannot redefine "threadCount"');

    config.passConnectionCache = config.passConnectionCache || false;
    assert.eq('boolean', typeof config.passConnectionCache);

    return config;
}
