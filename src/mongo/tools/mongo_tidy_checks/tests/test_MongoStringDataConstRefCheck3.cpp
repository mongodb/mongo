namespace mongo {
class StringData {};

int func0(StringData sd) {
    return 0;
}

int func1(StringData& sd) {
    return 0;
}

int func2(StringData&& sd) {
    return 0;
}

int func3(const StringData* sd) {
    return 0;
}
}  // namespace mongo

namespace testNS {
class StringData;

int func4(const testNS::StringData& sd) {
    return 0;
}

int func5(testNS::StringData& sd) {
    return 0;
}

}  // namespace testNS


template <typename T>
int func6(const T& sd) {
    return 0;
}

template <typename T>
int func7(const T&& sd) {
    return 0;
}

template <typename T>
int func8(T& sd) {
    return 0;
}

template <typename T>
int func9(T&& sd) {
    return 0;
}

#define SD_FUNCDEF(T)         \
    int func10(const T& sd) { \
        return 0;             \
    }

namespace mongo {
SD_FUNCDEF(StringData);

int TestSDCheckMain() {
    StringData sd;
    func6(sd);
    func7(StringData{});
    func8(sd);
    func9(StringData{});

    const StringData& sdRef = sd;
    func6(sdRef);

    return 0;
}
}  // namespace mongo
