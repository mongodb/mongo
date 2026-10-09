int crossSmokeLibrary();

extern "C" int crossLinkerSmoke() {
    return crossSmokeLibrary();
}
