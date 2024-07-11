template <typename T>
inline void invariantWithLocation(const T& testOK) {}

#define invariant(...) invariantWithLocation(__VA_ARGS__)

namespace mongo {
class Status {
public:
    bool isOK() {
        return true;
    }
};
void fun(Status status) {
    invariant(status.isOK());
}
}  // namespace mongo
