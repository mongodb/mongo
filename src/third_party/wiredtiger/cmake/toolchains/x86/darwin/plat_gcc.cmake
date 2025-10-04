# We are not cross-compiling if our system is Darwin, hence the "x86_64-apple-darwin-"
# prefix is not necessary when we are not cross-compiling. Just default to the host
# installed 'gcc' binary.
if(CMAKE_CROSSCOMPILING)
    set(CROSS_COMPILER_PREFIX "x86_64-apple-darwin-")
endif()
