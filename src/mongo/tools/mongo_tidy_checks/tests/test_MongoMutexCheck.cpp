#include <mutex>
namespace mongo {

namespace stdx {
using ::std::mutex;
}  // namespace stdx

// var Decl
stdx::mutex stdxmutex_vardecl;
std::mutex stdmutex_vardecl;

// field decl
struct structName {
    std::mutex stdmutex_fileddecl;
    stdx::mutex stdxmutex_fileddecl;
};

}  // namespace mongo
