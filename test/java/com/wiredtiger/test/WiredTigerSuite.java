package com.wiredtiger.test;

import org.junit.runner.RunWith;
import org.junit.runners.Suite;

@RunWith(Suite.class)
@Suite.SuiteClasses( {
    CursorTest.class,
    PackTest.class
})

public class WiredTigerSuite {
    // the class remains empty,
    // used only as a holder for the above annotations
}
