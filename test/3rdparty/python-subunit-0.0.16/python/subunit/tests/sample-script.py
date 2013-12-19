#!/usr/bin/env python
import sys
if sys.platform == "win32":
    import msvcrt, os
    msvcrt.setmode(sys.stdout.fileno(), os.O_BINARY)
if len(sys.argv) == 2:
    # subunit.tests.test_test_protocol.TestExecTestCase.test_sample_method_args 
    # uses this code path to be sure that the arguments were passed to
    # sample-script.py
    print("test fail")
    print("error fail")
    sys.exit(0)
print("test old mcdonald")
print("success old mcdonald")
print("test bing crosby")
print("failure bing crosby [")
print("foo.c:53:ERROR invalid state")
print("]")
print("test an error")
print("error an error")
sys.exit(0)
