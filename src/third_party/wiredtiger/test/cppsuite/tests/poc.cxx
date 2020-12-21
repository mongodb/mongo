#include <iostream>
#include <stdlib.h>

extern "C" {
    #include "wiredtiger.h"
}

int main(int argc, char *argv[]) {
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
