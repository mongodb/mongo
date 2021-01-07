#include <iostream>
#include <cstdlib>
#include "test_harness/test_harness.h"

class poc_test : public test_harness::test {
    public:
    int run() {
        WT_CONNECTION *conn;
        int ret = 0;
        /* Setup basic test directory. */
        const std::string default_dir = "WT_TEST";

        /*
        * Csuite tests utilise a test_util.h command to make their directory, currently that doesn't
        * compile under c++ and some extra work will be needed to make it work. Its unclear if the
        * test framework will use test_util.h yet.
        */
        const std::string mkdir_cmd = "mkdir " + default_dir;
        ret = system(mkdir_cmd.c_str());
        if (ret != 0)
            return (ret);

        ret = wiredtiger_open(default_dir.c_str(), NULL, "create,cache_size=1G", &conn);
        return (ret);
    }
};

int main(int argc, char *argv[]) {
    return poc_test().run();
}
