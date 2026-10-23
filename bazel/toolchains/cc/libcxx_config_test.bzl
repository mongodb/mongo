"""Test libc++ configuration for Apple's system runtime."""

load("@bazel_skylib//lib:unittest.bzl", "asserts", "unittest")
load(":libcxx_config.bzl", "enable_libcxx_vendor_availability")

def _vendor_availability_test_impl(ctx):
    env = unittest.begin(ctx)
    other_defines = "#define _LIBCPP_ABI_VERSION 1\n#define _LIBCPP_HAS_THREAD_API_PTHREAD\n"
    for disabled, enabled in [
        (
            "#define _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS",
            "/* Enable vendor availability annotations. */",
        ),
        (
            "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0",
            "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 1",
        ),
    ]:
        expected = other_defines + enabled + "\n"
        asserts.equals(env, expected, enable_libcxx_vendor_availability(other_defines + disabled + "\n"))
        asserts.equals(env, expected, enable_libcxx_vendor_availability(expected))
    asserts.equals(env, other_defines, enable_libcxx_vendor_availability(other_defines))
    return unittest.end(env)

libcxx_vendor_availability_test = unittest.make(_vendor_availability_test_impl)
