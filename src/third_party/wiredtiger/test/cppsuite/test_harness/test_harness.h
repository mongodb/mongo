/* Include guard. */
#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

/* Required to build using older versions of g++. */
#include <cinttypes>

/* Include various wiredtiger libs. */
#include "configuration_settings.h"
#include "wiredtiger.h"
#include "wt_internal.h"

namespace test_harness {
class test {
    public:
    /*
     * All tests will implement this initially, the return value from it will indicate whether the
     * test was successful or not.
     */
    virtual int run() = 0;

    test(const char *config)
    {
        _configuration = new configuration(_name, config);
    }

    ~test()
    {
        delete _configuration;
        _configuration = nullptr;
    }

    configuration *_configuration = nullptr;
    static const char *_name;
    static const char *_default_config;
};
} // namespace test_harness

#endif
