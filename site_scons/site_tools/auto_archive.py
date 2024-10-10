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
import sys

import SCons

PACKAGE_ALIAS_MAP = "AIB_PACKAGE_ALIAS_MAP"
AUTO_ARCHIVE_MAKE_ARCHIVE_CONTENT = """
import os
import sys

USAGE = '''
Usage: {} ARCHIVE_TYPE ARCHIVE_NAME ROOT_DIRECTORY FILES...

FILES should be absolute paths or relative to ROOT_DIRECTORY.

ARCHIVE_TYPE is one of zip or tar.
'''

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(sys.argv[0], "takes at minimum four arguments.")
        print(USAGE.format(sys.argv[0]))
        sys.exit(1)

    archive_type = sys.argv[1]
    archive_name = sys.argv[2]
    root_dir = sys.argv[3]
    file_list = sys.argv[4]

    files = []
    with open(file_list) as f:
        files = f.read().splitlines()

    if archive_type not in ("zip", "tar"):
        print("unsupported archive_type", archive_type)
        print(USAGE.format(sys.argv[0]))
        sys.exit(1)

    if archive_type == "zip":
        import zipfile
        archive = zipfile.ZipFile(archive_name, mode='w', compression=zipfile.ZIP_DEFLATED)
        add_file = archive.write
    else:
        import tarfile
        archive = tarfile.open(archive_name, mode='w:gz')
        add_file = archive.add

    os.chdir(root_dir)

    for filename in files:
        add_file(filename)

    archive.close()
"""


def add_package_name_alias(env, component, role, name):
    """Add a package name mapping for the combination of component and role."""
    # Verify we didn't get a None or empty string for any argument
    if not name:
        raise Exception("when setting a package name alias must provide a name parameter")
    if not component:
        raise Exception("No component provided for package name alias")
    if not role:
        raise Exception("No role provided for package name alias")
    env[PACKAGE_ALIAS_MAP][(component, role)] = name


def get_package_name(env, component, role):
    """Return the package file name for the component and role combination."""
    basename = env[PACKAGE_ALIAS_MAP].get(
        # TODO: silent roles shouldn't be included here
        (component, role),
        "{component}-{role}".format(component=component, role=role),
    )

    return basename


def collect_transitive_files(env, entry):
    """
    Collect all installed and transitively installed files for entry.
    """
    cache = set()
    files = []
    stack = [entry]

    # Find all the files directly contained in the component DAG for entry and
    # it's dependencies.
    while stack:
        s = stack.pop()
        if s in cache:
            continue
        cache.add(s)

        stack.extend(s.dependencies)
        files.extend(s.files)

    cache.clear()
    files, stack = stack, files

    # Now we will call the scanner to find the transtive files of any files that
    # we found from the component DAG.

    while stack:
        s = stack.pop()
        if s in cache:
            continue
        cache.add(s)

        files.append(s)
        # scan_for_transitive_install is memoized so it's safe to call it in
        # this loop. If it hasn't already run for a file we need to run it
        # anyway.
        stack.extend(env.GetTransitivelyInstalledFiles(s))

    # Setting the AIB_NO_ARCHIVE attribute to True prevents outputs from an
    # AutoInstall builder from being included into archives produced by this
    # tool
    # Usage:
    # node = env.AutoInstall(...)
    # setattr(node[0].attributes, 'AIB_NO_ARCHIVE', True)
    return sorted(f for f in files if not getattr(f.attributes, "AIB_NO_ARCHIVE", False))


def auto_archive_gen(first_env, make_archive_script, pkg_fmt):
    """Generate an archive task function for pkg_fmt where pkg_fmt is one of zip, tar, or auto."""

    if pkg_fmt == "auto":
        if first_env["PLATFORM"] == "win32":
            pkg_fmt = "zip"
        else:
            pkg_fmt = "tar"

    def auto_archive(env, component, role):
        pkg_name = get_package_name(env, component, role)
        install_alias = "install-{component}{role}".format(
            component=component,
            role="" if env.GetRoleDeclaration(role).silent else "-" + role,
        )

        if pkg_fmt == "zip":
            pkg_suffix = "$AUTO_ARCHIVE_ZIP_SUFFIX"
        else:
            pkg_suffix = "$AUTO_ARCHIVE_TARBALL_SUFFIX"

        archive = env.AutoArchive(
            target="$PKGDIR/{}.{}".format(pkg_name, pkg_suffix),
            source=[make_archive_script] + env.Alias(install_alias),
            __AUTO_ARCHIVE_TYPE=pkg_fmt,
            AIB_COMPONENT=component,
            AIB_ROLE=role,
        )

        # TODO: perhaps caching of packages / tarballs should be
        # configurable? It's possible someone would want to do it.
        env.NoCache(archive)
        return archive

    return auto_archive


def archive_builder(source, target, env, for_signature):
    """Build archives of the AutoInstall'd sources."""
    if not source:
        return []

    source = env.Flatten([source])
    common_ancestor = None

    # Get the path elements that make up both DESTDIR and PREFIX. Then
    # iterate the dest_dir_elems with the prefix path elements
    # stripped off the end of the path converting them to strings for
    # joining to make the common_ancestor.
    #
    # We pass the common_ancestor to tar via -C so that $PREFIX is
    # preserved in the tarball.
    common_ancestor = env.Dir("$DESTDIR")

    archive_type = env["__AUTO_ARCHIVE_TYPE"]
    make_archive_script = source[0]
    compression_flags = ""

    tar_cmd = env.WhereIs("tar")
    if archive_type == "tar" and tar_cmd:
        pigz_cmd = env.WhereIs("pigz")
        if pigz_cmd:
            # pigz is the parallel implementation of gizp,
            # it uses all available cores on the machine.
            # if available we use it to speedup compression.
            compression_flags = "--use-compress-program='{pigz_cmd}'".format(pigz_cmd=pigz_cmd)
        else:
            compression_flags = "-z"

        command_prefix = "{tar} -vc {compression_flags} -C {common_ancestor} -T {file_list} --ignore-failed-read -f {archive_name}"
    else:
        command_prefix = "{python} {make_archive_script} {archive_type} {archive_name} {common_ancestor} {file_list}"

    archive_name = env.File(target[0])
    command_sig = command_prefix.format(
        tar=tar_cmd,
        compression_flags=compression_flags,
        python=sys.executable,
        archive_type=archive_type,
        archive_name=archive_name,
        make_archive_script=make_archive_script,
        common_ancestor=common_ancestor,
        file_list="",
    )

    # If we are just being invoked for our signature, we can omit the indirect dependencies
    # found by expanding the transitive dependencies, since we really only have a hard dependency
    # on our direct dependencies.
    if for_signature:
        return command_sig

    component = env["AIB_COMPONENT"]
    role = env["AIB_ROLE"]
    entry = env["AIB_ALIAS_MAP"][component][role]

    # Pre-process what should be in the archive. We need to pass the
    # set of known installed files along to the transitive dependency
    # walk so we can filter out files that aren't in the install
    # directory.
    installed = set(env.FindInstalledFiles())

    # Collect all the installed files for our entry. This is doing a pure DAG
    # walk idea of what should be. So we filter out any that are not in the
    # installed set.
    transitive_files = [f for f in collect_transitive_files(env, entry) if f in installed]
    if not transitive_files:
        return []

    # TODO: relpath is costly, and we do it for every file in the archive here.
    # We should find a way to avoid the repeated relpath invocation, probably by
    # bucketing by directory.
    relative_files = [
        os.path.relpath(file.get_abspath(), common_ancestor.get_abspath())
        for file in transitive_files
    ]

    # This is not great for ninja, essentially we are doing realtime operations here, which is
    # not terrible for scons CommandActionGenerators, because the generation happens right before
    # executing the command. However, this means for ninja that the realtime things happen during
    # ninja generation, and are far removed from ninja execution. Even if we split this into a
    # separate action for ninja's sake, there would still be issues because the reason to make
    # such a filelist is to prevent creating command lines which are too long, and actions must
    # be converted to command lines for ninja. When the ninja tool is able to process scons
    # callbacks in order and not via aggregation then this could be moved to a simple Textfile call.
    file_list = str(target[0].abspath) + ".filelist"
    os.makedirs(os.path.dirname(file_list), exist_ok=True)
    with open(file_list, "w") as f:
        for file in relative_files:
            f.write(file + "\n")

    cmd = command_prefix.format(
        tar=tar_cmd,
        compression_flags=compression_flags,
        python=sys.executable,
        archive_type=archive_type,
        archive_name=archive_name,
        make_archive_script=make_archive_script,
        common_ancestor=common_ancestor,
        file_list=file_list,
    )

    return cmd


def exists(env):
    return True


def generate(env):
    if not env.get("AUTO_INSTALL_ENABLED"):
        env.Tool("auto_install_binaries")

    bld = SCons.Builder.Builder(
        action=SCons.Action.CommandGeneratorAction(
            archive_builder,
            {"cmdstr": "Building package ${TARGETS[0]} from ${SOURCES[1:]}"}
            if not env.Verbose()
            else {"cmdstr": ""},
        )
    )
    env.Append(BUILDERS={"AutoArchive": bld})
    env["AUTO_ARCHIVE_TARBALL_SUFFIX"] = env.get(
        "AUTO_ARCHIVE_TARBALL_SUFFIX",
        "tar.gz",
    )
    env["AUTO_ARCHIVE_ZIP_SUFFIX"] = env.get("AUTO_ARCHIVE_ZIP_SUFFIX", "zip")
    env[PACKAGE_ALIAS_MAP] = {}

    env.AddMethod(add_package_name_alias, "AddPackageNameAlias")

    # TODO: $BUILD_ROOT should be $VARIANT_DIR after we fix our dir
    # setup later on.
    make_archive_script = env.Textfile(
        target="$BUILD_ROOT/aib_make_archive.py",
        source=[AUTO_ARCHIVE_MAKE_ARCHIVE_CONTENT],
    )

    env.AppendUnique(
        AIB_TASKS={
            "tar": (auto_archive_gen(env, make_archive_script, "tar"), False),
            "zip": (auto_archive_gen(env, make_archive_script, "zip"), False),
            "archive": (auto_archive_gen(env, make_archive_script, "auto"), False),
        }
    )
