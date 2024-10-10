"""Dictionary to store available build profiles."""

import enum
from dataclasses import dataclass
from typing import Any, List, Optional

import mongo.generators as mongo_generators
from site_scons.mongo import platform


class BuildProfileType(str, enum.Enum):
    DEFAULT = "default"
    FAST = "fast"
    OPT = "opt"
    SAN = "san"
    TSAN = "tsan"
    COMPILE_DB = "compiledb"
    RELEASE = "release"


class BuildProfileNotSupported(Exception):
    pass


@dataclass
class BuildProfile:
    ninja: str
    variables_files: List
    allocator: str
    sanitize: Optional[str]
    link_model: str
    dbg: str
    opt: str
    ICECC: Optional[str]
    CCACHE: Optional[str]
    NINJA_PREFIX: str
    VARIANT_DIR: Any
    disable_warnings_as_errors: Optional[List]
    release: str
    jlink: float
    libunwind: str


def get_build_profile(type):
    os_name = platform.get_running_os_name()
    build_profile = _get_build_profile(type, os_name)

    if not build_profile:
        raise BuildProfileNotSupported(f"{type} is not supported on {os_name}")

    return build_profile


def _get_build_profile(type, os_name):
    if os_name == "windows":
        return WINDOWS_BUILD_PROFILES[type]
    elif os_name == "macOS":
        if platform.is_arm_processor():
            return MACOS_ARM_BUILD_PROFILES[type]
        else:
            return MACOS_BUILD_PROFILES[type]
    else:
        return LINUX_BUILD_PROFILES[type]


LINUX_BUILD_PROFILES = {
    # These options were the default settings before implementing build profiles.
    BuildProfileType.DEFAULT: BuildProfile(
        ninja="disabled",
        variables_files=["./etc/scons/mongodbtoolchain_stable_gcc.vars"],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="auto",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="build",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & fast build time at the cost of debuggability.
    BuildProfileType.FAST: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/mongodbtoolchain_stable_clang.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="dynamic",
        dbg="off",
        opt="off",
        ICECC="icecc",
        CCACHE="ccache",
        NINJA_PREFIX="fast",
        VARIANT_DIR="fast",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & debuggability at the cost of build time.
    BuildProfileType.OPT: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/mongodbtoolchain_stable_clang.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="dynamic",
        dbg="off",
        opt="on",
        ICECC="icecc",
        CCACHE="ccache",
        NINJA_PREFIX="opt",
        VARIANT_DIR="opt",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build leverages santizers & is the suggested build profile to use for development.
    BuildProfileType.SAN: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/mongodbtoolchain_stable_clang.vars",
        ],
        allocator="system",
        sanitize="undefined,address",
        link_model="dynamic",
        dbg="on",
        opt="debug",
        ICECC="icecc",
        CCACHE="ccache",
        NINJA_PREFIX="san",
        VARIANT_DIR="san",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build leverages thread sanitizers.
    BuildProfileType.TSAN: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/mongodbtoolchain_stable_clang.vars",
        ],
        allocator="system",
        sanitize="thread",
        link_model="dynamic",
        dbg="on",
        opt="on",
        ICECC="icecc",
        CCACHE="ccache",
        NINJA_PREFIX="tsan",
        VARIANT_DIR="tsan",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="off",
    ),
    # These options are the preferred settings for compiledb to generating compile_commands.json
    BuildProfileType.COMPILE_DB: BuildProfile(
        ninja="disabled",
        variables_files=[
            "./etc/scons/mongodbtoolchain_stable_clang.vars",
            "./etc/scons/developer_versions.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="dynamic",
        dbg="on",
        opt="off",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="compiledb",
        VARIANT_DIR="compiledb",
        disable_warnings_as_errors=["source"],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # These options were the default settings before implementing build profiles.
    BuildProfileType.RELEASE: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/mongodbtoolchain_stable_gcc.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="static",
        dbg="off",
        opt="on",
        ICECC="icecc",
        CCACHE="ccache",
        NINJA_PREFIX="release",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="on",
        jlink=0.01,
        libunwind="auto",
    ),
}

WINDOWS_BUILD_PROFILES = {
    # These options were the default settings before implementing build profiles.
    BuildProfileType.DEFAULT: BuildProfile(
        ninja="disabled",
        variables_files=[],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="auto",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="build",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & fast build time at the cost of debuggability.
    BuildProfileType.FAST: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="off",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="fast",
        VARIANT_DIR="fast",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & debuggability at the cost of build time.
    BuildProfileType.OPT: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="on",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="opt",
        VARIANT_DIR="opt",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build leverages santizers & is the suggested build profile to use for development.
    BuildProfileType.SAN: None,
    # This build leverages thread sanitizers.
    BuildProfileType.TSAN: None,
    # These options are the preferred settings for compiledb to generating compile_commands.json
    BuildProfileType.COMPILE_DB: BuildProfile(
        ninja="disabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="on",
        opt="off",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="compiledb",
        VARIANT_DIR="compiledb",
        disable_warnings_as_errors=["source"],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # These options were the default settings before implementing build profiles.
    BuildProfileType.RELEASE: BuildProfile(
        ninja="enabled",
        variables_files=[],
        allocator="auto",
        sanitize=None,
        link_model="static",
        dbg="off",
        opt="on",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="release",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="on",
        jlink=0.01,
        libunwind="auto",
    ),
}

MACOS_BUILD_PROFILES = {
    # These options were the default settings before implementing build profiles.
    BuildProfileType.DEFAULT: BuildProfile(
        ninja="disabled",
        variables_files=[],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="auto",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="build",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & fast build time at the cost of debuggability.
    BuildProfileType.FAST: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="off",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="fast",
        VARIANT_DIR="fast",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & debuggability at the cost of build time.
    BuildProfileType.OPT: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="on",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="opt",
        VARIANT_DIR="opt",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build leverages santizers & is the suggested build profile to use for development.
    BuildProfileType.SAN: None,
    # This build leverages thread sanitizers.
    BuildProfileType.TSAN: None,
    # These options are the preferred settings for compiledb to generating compile_commands.json
    BuildProfileType.COMPILE_DB: BuildProfile(
        ninja="disabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="on",
        opt="off",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="compiledb",
        VARIANT_DIR="compiledb",
        disable_warnings_as_errors=["source"],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # These options were the default settings before implementing build profiles.
    BuildProfileType.RELEASE: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="static",
        dbg="off",
        opt="on",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="release",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="on",
        jlink=0.01,
        libunwind="auto",
    ),
}

MACOS_ARM_BUILD_PROFILES = {
    # These options were the default settings before implementing build profiles.
    BuildProfileType.DEFAULT: BuildProfile(
        ninja="disabled",
        variables_files=[],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="auto",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="build",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & fast build time at the cost of debuggability.
    BuildProfileType.FAST: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="off",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="fast",
        VARIANT_DIR="fast",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build has fast runtime speed & debuggability at the cost of build time.
    BuildProfileType.OPT: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="off",
        opt="on",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="opt",
        VARIANT_DIR="opt",
        disable_warnings_as_errors=[],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # This build leverages santizers & is the suggested build profile to use for development.
    BuildProfileType.SAN: None,
    # This build leverages thread sanitizers.
    BuildProfileType.TSAN: None,
    # These options are the preferred settings for compiledb to generating compile_commands.json
    BuildProfileType.COMPILE_DB: BuildProfile(
        ninja="disabled",
        variables_files=[
            "./etc/scons/developer_versions.vars",
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="auto",
        dbg="on",
        opt="off",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="compiledb",
        VARIANT_DIR="compiledb",
        disable_warnings_as_errors=["source"],
        release="off",
        jlink=0.99,
        libunwind="auto",
    ),
    # These options were the default settings before implementing build profiles.
    BuildProfileType.RELEASE: BuildProfile(
        ninja="enabled",
        variables_files=[
            "./etc/scons/xcode_macosx_arm.vars",
        ],
        allocator="auto",
        sanitize=None,
        link_model="static",
        dbg="off",
        opt="on",
        ICECC=None,
        CCACHE=None,
        NINJA_PREFIX="release",
        VARIANT_DIR=mongo_generators.default_variant_dir_generator,
        disable_warnings_as_errors=[],
        release="on",
        jlink=0.01,
        libunwind="auto",
    ),
}
