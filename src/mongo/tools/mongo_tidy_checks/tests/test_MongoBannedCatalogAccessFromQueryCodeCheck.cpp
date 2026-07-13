#include <memory>

namespace mongo {

class AutoGetCollection {};

class CollectionCatalog {
public:
    static std::shared_ptr<CollectionCatalog> get() {
        return std::make_shared<CollectionCatalog>();
    }

    void foo() {};
};

int fun() {
    AutoGetCollection agc;
}

int fun2() {
    CollectionCatalog::get()->foo();
}

}  // namespace mongo
