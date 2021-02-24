#include <iostream>
#include <cstdlib>

#include "test_harness/test_harness.h"
#include "test_harness/workload_generator.h"

class poc_test : public test_harness::test {
    public:
    poc_test(const std::string &config, int64_t trace_level) : test(config)
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
            return (return_code);

        return_code = _wl->run();
        return (return_code);
    }

    private:
    test_harness::workload_generator *_wl = nullptr;
};

const std::string poc_test::test::_name = "poc_test";
const std::string poc_test::test::_default_config = "collection_count=2,key_count=5,value_size=20";
int64_t test_harness::workload_generator::_trace_level = 0;

int
main(int argc, char *argv[])
{
    std::string cfg = "";
    int64_t trace_level = 0;
    int64_t error_code = 0;

    // Parse args
    // -C   : Configuration
    // -t   : Trace level
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-C") {
            if ((i + 1) < argc)
                cfg = argv[++i];
            else {
                std::cerr << "No value given for option " << argv[i] << std::endl;
                return (-1);
            }
        } else if (std::string(argv[i]) == "-t") {
            if ((i + 1) < argc)
                trace_level = std::stoi(argv[++i]);
            else {
                std::cerr << "No value given for option " << argv[i] << std::endl;
                return (-1);
            }
        }
    }

    // Check if default configuration should be used
    if (cfg.empty())
        cfg = poc_test::test::_default_config;

    std::cout << "Configuration\t:" << cfg << std::endl;
    std::cout << "Tracel level\t:" << trace_level << std::endl;

    error_code = poc_test(cfg, trace_level).run();

    if (error_code == 0)
        std::cout << "SUCCESS" << std::endl;
    else
        std::cerr << "FAILED (Error code: " << error_code << ")" << std::endl;
    return error_code;
}
