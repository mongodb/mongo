#include <iostream>
#include <cstdlib>

#include "test_harness/test_harness.h"
#include "test_harness/workload_generator.h"

class poc_test : public test_harness::test {
    public:
    poc_test(const char *config, int64_t trace_level) : test(config)
    {
        test_harness::workload_generator::_trace_level = trace_level;
        _wl = new test_harness::workload_generator(_configuration);
    }

    ~poc_test()
    {
        delete _wl;
        _wl = nullptr;
    }

    int
    run()
    {
        int return_code = _wl->load();
        if (return_code != 0)
            throw std::runtime_error(
              "Load stage failed with error code: " + std::to_string(return_code));
        return_code = _wl->run();
        if (return_code != 0)
            throw std::runtime_error(
              "Run stage failed with error code: " + std::to_string(return_code));
        return return_code;
    }

    private:
    test_harness::workload_generator *_wl = nullptr;
};

const char *poc_test::test::_name = "poc_test";
const char *poc_test::test::_default_config = "collection_count=2,key_count=5,value_size=20";
int64_t test_harness::workload_generator::_trace_level = 0;

int
main(int argc, char *argv[])
{
    std::string cfg = "";
    int64_t trace_level = 0;

    // Parse args
    // -C   : Configuration
    // -t   : Trace level
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-C") == 0) {
            if (i + 1 < argc) {
                cfg = argv[++i];
            } else {
                throw std::invalid_argument("No value given for option " + std::string(argv[i]));
                std::cout << "No value given for option " << argv[i] << std::endl;
            }
        } else if (strcmp(argv[i], "-t") == 0) {
            if (i + 1 < argc) {
                trace_level = std::stoi(argv[++i]);
            } else {
                throw std::invalid_argument("No value given for option " + std::string(argv[i]));
            }
        }
    }

    // Check if default configuration should be used
    if (cfg.compare("") == 0) {
        std::cout << "Using default configuration" << std::endl;
        cfg = poc_test::test::_default_config;
    }

    std::cout << "Configuration\t:" << cfg << std::endl;
    std::cout << "Tracel level\t:" << trace_level << std::endl;

    return (poc_test(cfg.c_str(), trace_level).run());
}
