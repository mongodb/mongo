template <typename T>
inline void invariantWithLocation(const T& testOK) {}

#define invariant(...) invariantWithLocation(__VA_ARGS__)

namespace mongo {

void helperFun() {
    invariant(true);
}

class ShardingCoordinator {};

class RecoverableShardingDDLCoordinator : public ShardingCoordinator {
public:
    virtual void doSomething() = 0;
};

class MyCoordinator : public RecoverableShardingDDLCoordinator {
public:
    void doSomething() override;
};

}  // namespace mongo
