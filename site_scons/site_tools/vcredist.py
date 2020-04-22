# Copyright 2020 MongoDB Inc.
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files (the
# "Software"), to deal in the Software without restriction, including
# without limitation the rights to use, copy, modify, merge, publish,
# distribute, sublicense, and/or sell copies of the Software, and to
# permit persons to whom the Software is furnished to do so, subject to
# the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
# KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
# WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
# LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
# OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
# WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

import os
import re
import subprocess
import winreg

import SCons


def exists(env):
    result = "msvc" in env["TOOLS"]
    return result


# How to locate the Merge Modules path is described in:
#
# - VS2019: https://docs.microsoft.com/en-us/visualstudio/releases/2019/redistribution#visual-c-runtime-files
# - VS2017: https://docs.microsoft.com/en-us/visualstudio/productinfo/2017-redistribution-vs#visual-c-runtime-files
# - VS2015: https://docs.microsoft.com/en-us/visualstudio/productinfo/2015-redistribution-vs#visual-c-runtime
#
# However, please note that for VS2017 an VS2019, the documented paths are incorrect, per this
# discussion:
#
# - https://developercommunity.visualstudio.com/content/problem/828060/what-are-the-correct-location-to-search-for-vc-crt.html
#
# This tool uses the currently undocumented but correct paths.

# The keys are the values SCons accepts for TARGET_ARCH to name
# different windows targets, the values are the tag that VS uses
# for the associated redistributable for that platform.
#
# TODO: Expand this map as needed.
target_arch_expansion_map = {
    "amd64": "x64",
    "arm": None,
    "arm64": "arm64",
    "emt64": "x64",
    "i386": "x86",
    "x86": "x86",
    "x86_64": "x64",
}


def _get_programfiles():
    result = os.getenv("ProgramFiles(x86)")
    # When we run this under cygwin, the environment is broken, fall
    # back to hard coded C:\Program Files (x86)
    if result is None:
        result = "C:\\Program Files (x86)"
    if not os.path.isdir(result):
        return None
    return result


def _get_merge_module_name_for_feature(env, feature):
    version_components = env["MSVC_VERSION"].split(".")
    return "Microsoft_VC{msvc_major}{msvc_minor}_{feature}_{target_arch}.msm".format(
        msvc_major=version_components[0],
        msvc_minor=version_components[1],
        feature=feature,
        target_arch=target_arch_expansion_map[env.subst("$TARGET_ARCH")],
    )


def generate(env):
    if not exists(env):
        return

    env.Tool("msvc")

    env.AddMethod(_get_merge_module_name_for_feature, "GetMergeModuleNameForFeature")

    # Obtain the major and minor versions of the curently configured MSVC
    # and ensure that we are using a VC 14 based toolchain.
    #
    # Please see
    # https://en.wikipedia.org/wiki/Microsoft_Visual_C%2B%2B#Internal_version_numbering
    # for details on the various version numbers in play for
    # the Microsoft toolchain.
    msvc_major, msvc_minor = env["MSVC_VERSION"].split(".")
    if msvc_major != "14":
        return

    # We may or may not need to figure out the path to Program files,
    # depending on the various paths we take throught this logic.
    programfilesx86 = None

    # TODO: Getting this path is a start, but we should later provide
    # an abstraction over the names of the merge modules
    # themselves. They seem to have the form
    # Microsoft_VC{msvc_major}{msvc_minor}_{Feature}_{target_arch}.msm. It
    # would be useful to provide an env.MergeModuleNameFor('feature')
    # that consulted the values we have found here and used
    # TARGET_ARCH (normalized somehow) to select the right one.
    mergemodulepath = None

    # On VS2015 the merge modules are in the program files directory,
    # not under the VS install dir.
    if msvc_minor == "0":

        if not programfilesx86:
            programfilesx86 = _get_programfiles()
            if not programfilesx86:
                return

        mergemodulepath = os.path.join(programfilesx86, "Common Files", "Merge Modules")
        if os.path.isdir(mergemodulepath):
            env["MSVS"]["VCREDISTMERGEMODULEPATH"] = mergemodulepath

    if not "VSINSTALLDIR" in env["MSVS"]:

        # Compute a VS version based on the VC version. VC 14.0 is VS 2015, VC
        # 14.1 is VS 2017. Also compute the next theoretical version by
        # incrementing the major version by 1. Then form a range from this
        # that we can use as an argument to the -version flag to vswhere.
        vs_version = int(msvc_major) + int(msvc_minor)
        vs_version_next = vs_version + 1
        vs_version_range = "[{vs_version}.0, {vs_version_next}.0)".format(
            vs_version=vs_version, vs_version_next=vs_version_next
        )

        if not programfilesx86:
            programfilesx86 = _get_programfiles()
            if not programfilesx86:
                return

        # Use vswhere (it has a fixed stable path) to query where Visual Studio is installed.
        env["MSVS"]["VSINSTALLDIR"] = (
            subprocess.check_output(
                [
                    os.path.join(
                        programfilesx86,
                        "Microsoft Visual Studio",
                        "Installer",
                        "vswhere.exe",
                    ),
                    "-version",
                    vs_version_range,
                    "-property",
                    "installationPath",
                    "-nologo",
                ]
            )
            .decode("utf-8")
            .strip()
        )

    vsinstall_dir = env["MSVS"]["VSINSTALLDIR"]

    # Combine and set the full merge module path
    redist_root = os.path.join(vsinstall_dir, "VC", "Redist", "MSVC")
    if not os.path.isdir(redist_root):
        return
    env["MSVS"]["VCREDISTROOT"] = redist_root

    # Check the registry key that has the runtime lib version
    try:
        # TOOO: This x64 needs to be abstracted away. Is it the host
        # arch, or the target arch? My guess is host.
        vsruntime_key_name = "SOFTWARE\\Microsoft\\VisualStudio\\{msvc_major}.0\\VC\\Runtimes\\x64".format(
            msvc_major=msvc_major
        )
        vsruntime_key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, vsruntime_key_name)
        vslib_version, vslib_version_type = winreg.QueryValueEx(
            vsruntime_key, "Version"
        )
    except WindowsError:
        return

    # Fallback to directory search if we don't find the expected version
    redist_path = os.path.join(
        redist_root, re.match("v(\d+\.\d+\.\d+)\.\d+", vslib_version).group(1)
    )
    if not os.path.isdir(redist_path):
        redist_path = None
        dirs = os.listdir(redist_root)
        dirs.sort()
        for dir in reversed(dirs):
            candidate = os.path.join(redist_root, dir)
            if os.path.isdir(candidate):
                redist_path = candidate
                break
        else:
            return
    env["MSVS"]["VCREDISTPATH"] = redist_path

    if mergemodulepath is None and msvc_minor != "0":
        mergemodulepath = os.path.join(redist_path, "MergeModules")
        if os.path.isdir(mergemodulepath):
            env["MSVS"]["VCREDISTMERGEMODULEPATH"] = mergemodulepath

    # Keep these in preference order. The way with the {} in between
    # the dots appears to be the more modern form, but we select the
    # older one when available to minimize disruption to existing
    # automation that expects the redist executable embedded in our
    # packages to have this shape. Some architectures, like arm64,
    # don't appear to be provided under that syntax though, so we
    # include the newer form for that purpose. If Microsoft ever stops
    # providing the old form, we will automatically roll forward to
    # the new form.
    vcredist_search_template_sequence = [
        "vcredist_{}.exe",
        "vc_redist.{}.exe",
    ]

    expansion = target_arch_expansion_map.get(env.subst("$TARGET_ARCH"), None)
    if not expansion:
        return

    vcredist_candidates = [
        c.format(expansion) for c in vcredist_search_template_sequence
    ]
    for candidate in vcredist_candidates:
        candidate = os.path.join(redist_path, candidate)
        if os.path.isfile(candidate):
            break
    else:
        return
    env["MSVS"]["VCREDISTEXE"] = candidate
