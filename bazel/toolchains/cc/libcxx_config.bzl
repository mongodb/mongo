"""Configure LLVM libc++ headers for Apple's system runtime."""

def enable_libcxx_vendor_availability(config_site_content):
    """Enable availability guards for symbols missing from older Apple runtimes."""

    # LLVM 19 enables annotations when its negative macro is absent; newer
    # releases use a positive macro. Preserve headers already using the default.
    lines = []
    for line in config_site_content.split("\n"):
        stripped = line.strip()
        tokens = [token for token in stripped[1:].replace("\t", " ").split(" ") if token] if stripped.startswith("#") else []
        if len(tokens) >= 2 and tokens[0] == "define":
            if tokens[1] == "_LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS":
                line = "/* Enable vendor availability annotations. */"
            elif tokens[1] == "_LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS":
                if len(tokens) < 3 or tokens[2] not in ["0", "1"]:
                    fail("Unsupported libc++ vendor availability definition: " + line)
                if tokens[2] == "0":
                    line = "#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 1"
        lines.append(line)
    return "\n".join(lines)
