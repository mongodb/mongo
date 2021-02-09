#include <iostream>
#include <cstdlib>
#include "test_harness/test_harness.h"

class poc_test : public test_harness::test {
    public:
    int run() {
        WT_CONNECTION *conn;
        int ret = 0;
        /* Setup basic test directory. */
        const char *default_dir = "WT_TEST";

        /*
        * Csuite tests utilise a test_util.h command to make their directory, currently that doesn't
        * compile under c++ and some extra work will be needed to make it work. Its unclear if the
        * test framework will use test_util.h yet.
        */
        const char *mkdir_cmd = "mkdir WT_TEST";
        ret = system(mkdir_cmd);
        if (ret != 0)
            return (ret);

        ret = wiredtiger_open(default_dir, NULL, "create,cache_size=1G", &conn);
        return (ret);
    }

    poc_test(const char *config) : test(config) {}
};

const char *poc_test::test::_name = "poc_test";

int main(int argc, char *argv[]) {
    const char *cfg = "collection_count=1,key_size=5";
    return poc_test(cfg).run();
}
