// mongo/unittest/unittest_main.cpp

#include <string>
#include <vector>

#include "base/initializer.h"
#include "mongo/unittest/unittest.h"

int main( int argc, char **argv, char **envp ) {
    ::mongo::runGlobalInitializersOrDie(argc, argv, envp);
    return ::mongo::unittest::Suite::run(std::vector<std::string>(), "", 1);
}

void mongo::unittest::onCurrentTestNameChange( const std::string &testName ) {}
