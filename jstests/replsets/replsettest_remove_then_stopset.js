/**
 * Tests that it is safe to call stopSet() after a remove() in ReplSetTest.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";

const replTest = new ReplSetTest({nodes: 1});
replTest.startSet();
replTest.remove(0);
replTest.stopSet();