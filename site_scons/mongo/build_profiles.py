"""Dictionary to store available build profiles."""
from dataclasses import dataclass
from typing import Any, List, Optional
import mongo.generators as mongo_generators


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


BUILD_PROFILES = {
    # These options were the default settings before implementing build profiles.
    "default":
        BuildProfile(
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
        ),
    # This build has fast runtime speed & fast build time at the cost of debuggability.
    "fast":
        BuildProfile(
            ninja="enabled",
            variables_files=[
                './etc/scons/mongodbtoolchain_stable_clang.vars',
                './etc/scons/developer_versions.vars',
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
        ),
    # This build has fast runtime speed & debuggability at the cost of build time.
    "opt":
        BuildProfile(
            ninja="enabled",
            variables_files=[
                './etc/scons/mongodbtoolchain_stable_clang.vars',
                './etc/scons/developer_versions.vars',
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
        ),
    # This build leverages santizers & is the suggested build profile to use for development.
    "san":
        BuildProfile(
            ninja="enabled",
            variables_files=[
                './etc/scons/mongodbtoolchain_stable_clang.vars',
                './etc/scons/developer_versions.vars',
            ],
            allocator="system",
            sanitize="undefined,address",
            link_model="dynamic",
            dbg="on",
            opt="off",
            ICECC="icecc",
            CCACHE="ccache",
            NINJA_PREFIX="san",
            VARIANT_DIR="san",
        ),
}
