TestBinaryInfo = provider(
    doc = "Test binaries and their data returned by this target.",
    fields = {
        "test_binaries": "group of all test binaries",
        "test_data": "runfiles data required by all test binaries",
    },
)
