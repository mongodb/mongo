namespace mongo {
class StringData {
public:
    StringData() = default;
    StringData(const char*);

    const char* data() const;
    bool starts_with(StringData) const;
    bool ends_with(StringData) const;

    const char* rawData() const;
    bool startsWith(StringData) const;
    bool endsWith(StringData) const;
};

namespace {
int test() {
    StringData sd;
    sd.rawData();
    sd.endsWith("a");
    sd.startsWith("a");
    return 0;
}
}  // namespace
}  // namespace mongo
