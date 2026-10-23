"""Configure LLVM libc++ headers for Apple's system runtime."""

def enable_libcxx_vendor_availability(config_site_content):
    """Enable availability guards for symbols missing from older Apple runtimes."""

    # LLVM 19 uses a negative macro; newer releases use a positive macro.
    return config_site_content.replace(
        "#define _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS",
        "/* Enable vendor availability annotations. */",
    ).replace(
        "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0",
        "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 1",
    )
