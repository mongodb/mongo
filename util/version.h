#ifndef UTIL_VERSION_HEADER
#define UTIL_VERSION_HEADER

#include <string>

namespace mongo {
    struct BSONArray;

    using std::string;

    // mongo version
    extern const char versionString[];
    extern const BSONArray versionArray;
    string mongodVersion();
    int versionCmp(StringData rhs, StringData lhs); // like strcmp

    const char * gitVersion();
    void printGitVersion();

    string sysInfo();
    void printSysInfo();

    void show_warnings();

}  // namespace mongo

#endif  // UTIL_VERSION_HEADER
