/**
 * Test that oplogApplicationEnforcesSteadyStateConstraints is enabled by default in resmoke test
 * fixtures.
 */

const testName = jsTestName();
const rst = new ReplSetTest({name: testName, nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

assert(primary.adminCommand({getParameter: 1,
                             oplogApplicationEnforcesSteadyStateConstraints:
                                 1})["oplogApplicationEnforcesSteadyStateConstraints"]);

assert(secondary.adminCommand({getParameter: 1,
                               oplogApplicationEnforcesSteadyStateConstraints:
                                   1})["oplogApplicationEnforcesSteadyStateConstraints"]);

rst.stopSet();
