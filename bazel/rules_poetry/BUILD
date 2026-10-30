load("@rules_python//python:defs.bzl", "py_test")
load("@rules_poetry//rules_poetry:defs.bzl", "download_wheel", "pip_install")
load(":wheel_selection_test.bzl", "direct_source_rejected_test", "download_args_test", "install_environment_test", "variants_test")

variants_test(name = "hosted_wheel_variants_test")

download_wheel(
    name = "cryptography_download_fixture",
    hashes = ["sha256:" + "a" * 64],
    marker = "",
    only_binary = True,
    pkg = "cryptography",
    requirements = ["cryptography @ https://example.com/cryptography.whl --hash=sha256:" + "a" * 64],
    source_url = "",
    tags = ["manual"],
    version = "44.0.2",
)

download_wheel(
    name = "other_package_download_fixture",
    hashes = ["sha256:" + "a" * 64],
    marker = "",
    pkg = "avro-python3",
    source_url = "",
    tags = ["manual"],
    version = "1.10.2",
)

download_wheel(
    name = "cryptography_direct_source_fixture",
    hashes = ["sha256:" + "a" * 64],
    marker = "",
    only_binary = True,
    pkg = "cryptography",
    requirements = ["cryptography @ https://example.com/cryptography.tar.gz --hash=sha256:" + "a" * 64],
    source_url = "",
    tags = ["manual"],
    version = "44.0.2",
)

download_args_test(
    name = "cryptography_wheel_only_test",
    expected_wheel_only = True,
    target_under_test = ":cryptography_download_fixture",
)

download_args_test(
    name = "other_package_source_fallback_test",
    expected_wheel_only = False,
    target_under_test = ":other_package_download_fixture",
)

direct_source_rejected_test(
    name = "cryptography_direct_source_rejected_test",
    target_under_test = ":cryptography_direct_source_fixture",
)

exports_files(["python_build_environment.py"])

py_test(
    name = "python_build_environment_test",
    srcs = [
        "python_build_environment.py",
        "python_build_environment_test.py",
    ],
    imports = ["."],
    tags = ["unit"],
)

pip_install(
    name = "install_environment_fixture",
    tags = ["manual"],
    wheel = ":other_package_download_fixture",
)

install_environment_test(
    name = "install_environment_test",
    target_under_test = ":install_environment_fixture",
)
