#ifndef UTIL_VERSION_HEADER
#define UTIL_VERSION_HEADER

#include <string>

namespace mongo {

    using std::string;

    // mongo version
    extern const char versionString[];
    string mongodVersion();    

    const char * gitVersion();
    void printGitVersion();

    string sysInfo();
    void printSysInfo();

    void show_warnings();

}  // namespace mongo

#endif  // UTIL_VERSION_HEADER
