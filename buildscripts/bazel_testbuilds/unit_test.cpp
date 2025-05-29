// C++ compilation file used for testing bazel compilation.

#include <iostream>

#include <dlfcn.h>

int main() {
    void* tcmalloc_so = dlopen(
        "/home/ubuntu/mongo/bazel-bin/_solib_arm64/"
        "libsrc_Sthird_Uparty_Sgperftools_Slibtcmalloc_Uminimal.so",
        RTLD_NOW);
    void* tcmalloc = dlsym(tcmalloc_so, "malloc");

    void* libcmalloc_so = dlopen("/lib/aarch64-linux-gnu/libc.so.6", RTLD_NOW);
    void* libcmalloc = dlsym(libcmalloc_so, "malloc");

    printf("%p [unit test malloc]\n", malloc);
    printf("%p [tcmalloc]\n", tcmalloc);
    printf("%p [libc malloc]\n", libcmalloc);
    return 0;
}
