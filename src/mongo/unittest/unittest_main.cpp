// mongo/unittest/unittest_main.cpp

#include <string>
#include <vector>

#include "mongo/unittest/unittest.h"

int main( int argc, char **argv ) {
    return ::mongo::unittest::Suite::run(std::vector<std::string>(), "");
}

void mongo::unittest::onCurrentTestNameChange( const std::string &testName ) {}
