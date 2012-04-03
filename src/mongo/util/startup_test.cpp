// mongo/util/startup_test.cpp

#include "mongo/util/startup_test.h"

namespace mongo {
    std::vector<StartupTest*> *StartupTest::tests = 0;
    bool StartupTest::running = false;

    StartupTest::StartupTest() {
        registerTest(this);
    }

    StartupTest::~StartupTest() {}

    void StartupTest::registerTest( StartupTest *t ) {
        if ( tests == 0 )
            tests = new std::vector<StartupTest*>();
        tests->push_back(t);
    }

    void StartupTest::runTests() {
        running = true;
        for ( std::vector<StartupTest*>::const_iterator i = tests->begin();
              i != tests->end(); i++ ) {

            (*i)->run();
        }
        running = false;
    }

}  // namespace mongo
