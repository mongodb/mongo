namespace mongo {

class TracerProvider {
public:
    static void initialize() {}
    static TracerProvider get() {
        return TracerProvider();
    }
};

int testTraceProviderClass() {
    TracerProvider::initialize();
    TracerProvider provider = TracerProvider::get();
    return 0;
}

}  // namespace mongo
