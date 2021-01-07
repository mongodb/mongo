/* Include guard. */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

extern "C" {
#include "wiredtiger.h"
}

namespace test_harness {
class test {
    public:
    /*
     * All tests will implement this initially, the return value from it will indicate whether the
     * test was successful or not.
     */
    virtual int run() = 0;
};
} // namespace test_harness

#endif
