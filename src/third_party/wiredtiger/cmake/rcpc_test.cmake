include(CheckCSourceCompiles)

# Check whether the system we are building for or the user provided -mcpu supports ldapr. If the
# user did provide -mcpu it will results in a switch conflict warning which we catch with the
# failure regex argument. If the user didn't provide that then the compiler should detect whether
# the ldapr instruction is supported or not and either compile or fail to compile the snippet.
#
# Do this in a function to avoid the required flags variable leaving this scope.
function(rcpc_test)
set(CMAKE_REQUIRED_FLAGS -march=armv8.2-a+rcpc+crc)
    check_c_source_compiles("
        int main(void) {
            int a;
            int *b;
            int c;
            b = &a;
            __asm__ volatile(\"ldapr %0, %1\" : \"=r\"(c) : \"Q\"(b));
            return 0;
        }"
        HAVE_RCPC
        FAIL_REGEX "switch.*conflicts with.*switch"
    )
endfunction()

cmake_language(CALL rcpc_test)

if(HAVE_RCPC)
    message(DEBUG "Machine supports RCPC, Setting HAVE_RCPC")
endif()
