#!/usr/bin/env python3
"""generate_sbom.py config. Operational configuration values stored separately from the core code."""

import logging
import re

logger = logging.getLogger("generate_sbom")
logger.setLevel(logging.NOTSET)


# ################ Component Filters ################

# List of Endor Labs SBOM components that must be removed before processing
endor_components_remove = []

# bom-ref prefixes (Endor Labs has been changing them, so add all that we have seen)
prefixes = [
    "pkg:c/github.com/",
    "pkg:generic/github.com/",
    "pkg:github/",
]

components_remove = [
    # Endor Labs includes the main component in 'components'. This is not standard, so we remove it.
    "10gen/mongo",
    # should be pkg:github/antirez/linenoise - waiting on Endor Labs fix
    "amokhuginnsson/replxx",
    # a transitive dependency of s2 that is not necessary to include
    "sparsehash/sparsehash",
]

for component in components_remove:
    for prefix in prefixes:
        endor_components_remove.append(prefix + component)

# ################ Component Renaming ################
# Endor does not have syntactically valid PURLs for C/C++ packages.
# e.g.,
# Invalid: pkg:c/github.com/abseil/abseil-cpp@20250512.1
# Valid: pkg:github/abseil/abseil-cpp@20250512.1
# Run string replacements to correct for this:
endor_components_rename = [
    ["pkg:c/sourceware.org/git/valgrind", "pkg:generic/valgrind/valgrind"],
    ["pkg:generic/sourceware.org/git/valgrind", "pkg:generic/valgrind/valgrind"],
    ["pkg:generic/zlib.net/zlib", "pkg:github/madler/zlib"],
    ["pkg:generic/tartarus.org/libstemmer", "pkg:github/snowballstem/snowball"],
    ["pkg:generic/intel.com/intel-dfp-math", "pkg:generic/intel/IntelRDFPMathLib"],
    ["pkg:c/git.openldap.org/openldap/openldap", "pkg:generic/openldap/openldap"],
    ["pkg:generic/github.com/", "pkg:github/"],
    ["pkg:c/github.com/", "pkg:github/"],
]

# ################ Version Transformation ################

# In some cases we need to transform the version string to strip out tag-related text
# It is unknown what patterns may appear in the future, so we have targeted (not broad) regex
# This a list of 'pattern' and 'repl' inputs to re.sub()
RE_VER_NUM = r"(0|[1-9]\d*)"
RE_VER_LBL = r"(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?"
RE_SEMVER = rf"{RE_VER_NUM}\.{RE_VER_NUM}\.{RE_VER_NUM}{RE_VER_LBL}"
regex_semver = re.compile(RE_SEMVER)

VERSION_PATTERN_REPL = [
    # 'debian/1.28.1-1' pkg:github/mongodb/mongo-c-driver (temporary workaround)
    [re.compile(rf"^debian/({RE_SEMVER})-\d$"), r"\1"],
    # 'gperftools-2.9.1' pkg:github/gperftools/gperftools
    # 'mongo/v1.5.2' pkg:github/google/benchmark
    # 'mongodb-8.2.0-alpha2' pkg:github/wiredtiger/wiredtiger
    # 'release-1.12.0' pkg:github/apache/avro
    # 'yaml-cpp-0.6.3' pkg:github/jbeder/yaml-cpp
    [re.compile(rf"^[-a-z]+[-/][vr]?({RE_SEMVER})$"), r"\1"],
    # 'asio-1-34-2' pkg:github/chriskohlhoff/asio
    # 'cares-1_27_0' pkg:github/c-ares/c-ares
    [
        re.compile(rf"^[a-z]+-{RE_VER_NUM}[_-]{RE_VER_NUM}[_-]{RE_VER_NUM}{RE_VER_LBL}$"),
        r"\1.\2.\3",
    ],
    # 'pcre2-10.40' pkg:github/pcre2project/pcre2
    [re.compile(rf"^[a-z0-9]+-({RE_VER_NUM}\.{RE_VER_NUM})$"), r"\1"],
    # 'icu-release-57-1' pkg:github/unicode-org/icu
    [re.compile(rf"^[a-z]+-?[a-z]+-{RE_VER_NUM}-{RE_VER_NUM}$"), r"\1.\2"],
    # 'v2.6.0'  pkg:github/confluentinc/librdkafka
    # 'r2.5.1'
    [re.compile(rf"^[rv]({RE_SEMVER})$"), r"\1"],
    # 'v2025.04.21.00' pkg:github/facebook/folly
    [re.compile(r"^v(\d+\.\d+\.\d+\.\d+)$"), r"\1"],
]


def get_semver_from_release_version(release_ver: str) -> str:
    """Extract the version number from string with tags or other annotations"""
    if release_ver:
        for re_obj, repl in VERSION_PATTERN_REPL:
            if re_obj.match(release_ver):
                return re_obj.sub(repl, release_ver)
    return release_ver


# region special component use-case functions


def get_version_from_wiredtiger_release_info(wt_dir: str) -> str:
    """Get version from 'RELEASE_INFO' file in the wiredtiger folder"""

    import os

    v = {}
    try:
        for l in open(os.path.join(wt_dir, "RELEASE_INFO"), "r", encoding="utf-8"):
            if re.match(r"WIREDTIGER_VERSION_(?:MAJOR|MINOR|PATCH)=", l):
                exec(l, v)
        wt_ver = "%d.%d.%d" % (
            v["WIREDTIGER_VERSION_MAJOR"],
            v["WIREDTIGER_VERSION_MINOR"],
            v["WIREDTIGER_VERSION_PATCH"],
        )
        return wt_ver
    except Exception as e:
        logger.error(f"Error loading file from {wt_dir}")
        logger.error(e)
        return None


def get_version_sasl_from_workspace(file_path: str) -> str:
    """Determine the version that is pulled for Windows Cyrus SASL by searching WORKSPACE.bazel"""
    # e.g.,
    #         "https://s3.amazonaws.com/boxes.10gen.com/build/windows_cyrus_sasl-2.1.28.zip",
    try:
        with open(file_path, "r") as file:
            for line in file:
                if line.strip().startswith(
                    '"https://s3.amazonaws.com/boxes.10gen.com/build/windows_cyrus_sasl-'
                ):
                    return line.strip().split("windows_cyrus_sasl-")[1].split(".zip")[0]
    except Exception as e:
        logger.warning(f"Unable to load {file_path}")
        logger.warning(e)
    else:
        return None


def process_component_special_cases(
    component_key: str, component: dict, versions: dict, repo_root: str
) -> None:
    ## Special case for Cyrus SASL ##
    if component_key == "pkg:github/cyrusimap/cyrus-sasl":
        # Cycrus SASL is optionally loaded as a Windows library, when needed. There is no source code for Endor Labs to scan.
        # The version of Cyrus SASL that is used is defined in the WORKSPACE.bazel file:
        #         "https://s3.amazonaws.com/boxes.10gen.com/build/windows_cyrus_sasl-2.1.28.zip",
        # Rather than add the complexity of Bazel queries to this script, we just search the text.

        versions["import_script"] = get_version_sasl_from_workspace(repo_root + "/WORKSPACE.bazel")
        logger.info(
            f"VERSION SPECIAL CASE: {component_key}: Found version '{versions['import_script']}' in 'WORKSPACE.bazel' file"
        )

    ## Special case for wiredtiger ##
    elif component_key == "pkg:github/wiredtiger/wiredtiger":
        # MongoDB release branches import wiredtiger commits via a bot. These commits will likely not line up with a release or tag.
        # Endor labs will try to pull the nearest release/tag, but we want the more precise commit hash, which is stored in:
        # src/third_party/wiredtiget/import.data
        occurrences = component.get("evidence", {}).get("occurrences", [])
        if occurrences:
            location = occurrences[0].get("location")
            versions["import_script"] = get_version_from_wiredtiger_release_info(
                f"{repo_root}/{location}"
            )
            logger.info(
                f"VERSION SPECIAL CASE: {component_key}: Found version '{versions['import_script']}' in 'RELEASE_INFO' file"
            )

# endregion special component use-case functions
