#include <atomic>

namespace mongo {

// Variable Decl
std::atomic<int> atomic_var;

// Field Decl
struct structName {
    std::atomic<int> field_decl;
};

}  // namespace mongo
