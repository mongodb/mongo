[private]
default:
    @cd {{ invocation_directory() }}; just --choose

_mk-dir name:
    rm -rf {{ name }}
    mkdir {{ name }}

build-valgrind-path := "build-valgrind-" + os() + "-" + arch()

# Create a build directory for a Debug build without ASAN, so that valgrind can work
mk-build-valgrind: (_mk-dir build-valgrind-path)
    cd {{ build-valgrind-path }} ; cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -Dimmer_BUILD_TESTS=ON -Dimmer_BUILD_PERSIST_TESTS=ON -Dimmer_BUILD_EXAMPLES=OFF -DCXX_STANDARD=17

[linux]
run-valgrind:
    cd {{ build-valgrind-path }} ; ninja tests && ctest -D ExperimentalMemCheck

[linux]
run-valgrind-persist:
    cd {{ build-valgrind-path }} ; ninja persist-tests && valgrind --quiet --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=all \
                --suppressions=../test/extra/persist/valgrind.supp \
                ./test/extra/persist/persist-tests

build-asan-path := "build-asan-" + os() + "-" + arch()

# Create a build directory for a Debug build with ASAN enabled
mk-build-asan: (_mk-dir build-asan-path)
    cd {{ build-asan-path }} ; cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON -Dimmer_BUILD_TESTS=ON -Dimmer_BUILD_PERSIST_TESTS=ON -Dimmer_BUILD_EXAMPLES=OFF -DCXX_STANDARD=17

run-tests-asan:
    cd {{ build-asan-path }} ; ninja tests && ninja test

build-docs-path := "build-docs-" + os() + "-" + arch()

[linux]
mk-build-docs: (_mk-dir build-docs-path)
    rm -rf doc/_build
    rm -rf doc/_doxygen
    cmake -B {{ build-docs-path }} -G Ninja -Dimmer_BUILD_TESTS=off

[linux]
build-docs:
    cmake --build {{ build-docs-path }} --target docs
