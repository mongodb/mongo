#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <cinttypes>

#include "configuration_settings.h"
#include "wiredtiger.h"
#include "wt_internal.h"

namespace test_harness {
class test {
    public:
    test(const std::string &config)
    {
        _configuration = new configuration(_name, config);
    }

    ~test()
    {
        delete _configuration;
        _configuration = nullptr;
    }

    /*
     * All tests will implement this initially, the return value from it will indicate whether the
     * test was successful or not.
     */
    virtual int run() = 0;

    configuration *_configuration = nullptr;
    static const std::string _name;
    static const std::string _default_config;
};
} // namespace test_harness

#endif
