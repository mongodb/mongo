#include <string>
namespace mongo {
class CollectionShardingRuntime {
public:
    // Constructor
    CollectionShardingRuntime(int a, std::string s) : value(a), name(s) {}

    // Static function
    static int functionTest(int a, std::string s) {
        return a + s.length();
    }

private:
    int value;
    std::string name;
};


int testMongoUnstructuredLogFuncton1111() {
    // Create an instance of the object
    CollectionShardingRuntime csr(5, "Test");

    // Call the static function
    int result = CollectionShardingRuntime::functionTest(7, "Test");
}

}  // namespace mongo
