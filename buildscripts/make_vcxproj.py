"""Generate vcxproj and vcxproj.filters files for browsing code in Visual Studio 2015.

To build mongodb, you must use bazel. You can use this project to navigate code during debugging.

 HOW TO USE

 First, you need a compile_commands.json file, to generate run the following command:
   bazel build compiledb

 Next, run the following command
   python buildscripts/make_vcxproj.py FILE_NAME

  where FILE_NAME is the of the file to generate e.g., "mongod"
"""

import argparse
import io
import json
import os
import uuid
import xml.etree.ElementTree as ET

VCXPROJ_FOOTER = r"""

  <ItemGroup>
    <None Include="src\mongo\db\mongo.ico" />
  </ItemGroup>

  <ItemGroup>
    <ResourceCompile Include="src\mongo\db\db.rc" />
  </ItemGroup>

  <ItemGroup>
    <Natvis Include="buildscripts\win\mongodb.natvis" />
    <Natvis Include="src\third_party\yaml-cpp\yaml-cpp\src\contrib\yaml-cpp.natvis" />
  </ItemGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets"></ImportGroup>
</Project>
"""

VCXPROJ_NAMESPACE = "http://schemas.microsoft.com/developer/msbuild/2003"

# We preserve certain fields by saving them and restoring between file generations
VCXPROJ_FIELDS_TO_PRESERVE = [
    "NMakeBuildCommandLine",
    "NMakeOutput",
    "NMakeCleanCommandLine",
    "NMakeReBuildCommandLine",
]

VCXPROJ_TOOLSVERSION = {
    "14.1": "15.0",
    "14.2": "16.0",
    "14.3": "17.0",
}

VCXPROJ_PLATFORM_TOOLSET = {
    "14.1": "v141",
    "14.2": "v142",
    "14.3": "v143",
}

VCXPROJ_WINDOWS_TARGET_SDK = {
    "14.1": "10.0.17763.0",
    "14.2": "10.0.18362.0",
    "14.3": "10.0",
}

VCXPROJ_MSVC_DEFAULT_VERSION = "14.3"  # Visual Studio 2022


def get_defines(args):
    """Parse a compiler argument list looking for defines."""
    ret = set()
    for arg in args:
        if arg.startswith("/D"):
            ret.add(arg[2:])
    return ret


def get_includes(args):
    """Parse a compiler argument list looking for includes."""
    ret = set()
    for arg in args:
        if arg.startswith("/I"):
            ret.add(arg[2:])
    return ret


def _read_vcxproj(file_name):
    """Parse a vcxproj file and look for "NMake" prefixed elements in PropertyGroups."""

    # Skip if this the first run
    if not os.path.exists(file_name):
        return None

    tree = ET.parse(file_name)

    interesting_tags = ["{%s}%s" % (VCXPROJ_NAMESPACE, tag) for tag in VCXPROJ_FIELDS_TO_PRESERVE]

    save_elements = {}

    for parent in tree.getroot():
        for child in parent:
            if child.tag in interesting_tags:
                cond = parent.attrib["Condition"]
                save_elements[(parent.tag, child.tag, cond)] = child.text

    return save_elements


def _replace_vcxproj(file_name, restore_elements):
    """Parse a vcxproj file, and replace elememts text nodes with values saved before."""
    # Skip if this the first run
    if not restore_elements:
        return

    tree = ET.parse(file_name)

    interesting_tags = ["{%s}%s" % (VCXPROJ_NAMESPACE, tag) for tag in VCXPROJ_FIELDS_TO_PRESERVE]

    for parent in tree.getroot():
        for child in parent:
            if child.tag in interesting_tags:
                # Match PropertyGroup elements based on their condition
                cond = parent.attrib["Condition"]
                saved_value = restore_elements[(parent.tag, child.tag, cond)]
                child.text = saved_value

    stream = io.StringIO()

    tree.write(stream, encoding="unicode")

    str_value = stream.getvalue()

    # Strip the "ns0:" namespace prefix because ElementTree does not support default namespaces.
    str_value = (
        str_value.replace("<ns0:", "<").replace("</ns0:", "</").replace("xmlns:ns0", "xmlns")
    )

    with io.open(file_name, mode="w") as file_handle:
        file_handle.write(str_value)


class ProjFileGenerator(object):
    """Generate a .vcxproj and .vcxprof.filters file."""

    def __init__(self, target, vs_version):
        """Initialize ProjFileGenerator."""
        # we handle DEBUG in the vcxproj header:
        self.common_defines = set()
        self.common_defines.add("DEBUG")
        self.common_defines.add("_DEBUG")

        self.includes = set()
        self.target = target
        self.compiles = []
        self.files = set()
        self.all_defines = set()
        self.vcxproj = None
        self.filters = None
        self.all_defines = set(self.common_defines)
        self.vcxproj_file_name = self.target + ".vcxproj"
        self.tools_version = VCXPROJ_TOOLSVERSION[vs_version]
        self.platform_toolset = VCXPROJ_PLATFORM_TOOLSET[vs_version]
        self.windows_target_sdk = VCXPROJ_WINDOWS_TARGET_SDK[vs_version]

        self.existing_build_commands = _read_vcxproj(self.vcxproj_file_name)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.vcxproj = open(
            self.vcxproj_file_name,
            "w",
        )

        with open("buildscripts/vcxproj.header", "r") as header_file:
            header_str = header_file.read()
            header_str = header_str.replace("%_TARGET_%", self.target)
            header_str = header_str.replace("%ToolsVersion%", self.tools_version)
            header_str = header_str.replace("%PlatformToolset%", self.platform_toolset)
            header_str = header_str.replace(
                "%WindowsTargetPlatformVersion%", self.windows_target_sdk
            )
            header_str = header_str.replace(
                "%AdditionalIncludeDirectories%", ";".join(sorted(self.includes))
            )
            self.vcxproj.write(header_str)

        common_defines = self.all_defines
        for comp in self.compiles:
            common_defines = common_defines.intersection(comp["defines"])

        self.vcxproj.write("<!-- common_defines -->\n")
        self.vcxproj.write(
            "<ItemDefinitionGroup><ClCompile><PreprocessorDefinitions>"
            + ";".join(common_defines)
            + ";%(PreprocessorDefinitions)\n"
        )
        self.vcxproj.write("</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>\n")

        self.vcxproj.write("  <ItemGroup>\n")
        for command in self.compiles:
            defines = command["defines"].difference(common_defines)
            if defines:
                self.vcxproj.write(
                    '    <ClCompile Include="'
                    + command["file"]
                    + '"><PreprocessorDefinitions>'
                    + ";".join(defines)
                    + ";%(PreprocessorDefinitions)"
                    + "</PreprocessorDefinitions></ClCompile>\n"
                )
            else:
                self.vcxproj.write('    <ClCompile Include="' + command["file"] + '" />\n')
        self.vcxproj.write("  </ItemGroup>\n")

        self.filters = open(self.target + ".vcxproj.filters", "w")
        self.filters.write("<?xml version='1.0' encoding='utf-8'?>\n")
        self.filters.write(
            "<Project ToolsVersion='14.0' "
            + "xmlns='http://schemas.microsoft.com/developer/msbuild/2003'>\n"
        )

        self.__write_filters()

        self.vcxproj.write(VCXPROJ_FOOTER)
        self.vcxproj.close()

        self.filters.write("</Project>\n")
        self.filters.close()

        # Replace build commands
        _replace_vcxproj(self.vcxproj_file_name, self.existing_build_commands)

    def parse_args(self, args):
        """Parse a build line."""
        self.__parse_cl_line(args[1:])

    def __parse_cl_line(self, args):
        """Parse a compiler line."""
        # Get the file we are compilong
        file_name = None
        prev_arg = None
        for arg in reversed(args):
            if not file_name:
                if arg == "/c":
                    file_name = prev_arg
                elif arg.startswith("/c"):
                    file_name = arg[2:]

            if file_name:
                break
            prev_arg = arg

        if file_name not in self.files:
            self.files.add(file_name)

            file_defines = set()
            for arg in get_defines(args):
                if arg not in self.common_defines:
                    file_defines.add(arg)
            self.all_defines = self.all_defines.union(file_defines)

            for arg in get_includes(args):
                self.includes.add(arg)

            self.compiles.append({"file": file_name, "defines": file_defines})

    @staticmethod
    def __is_header(name):
        """Return True if this a header file."""
        headers = [".h", ".hpp", ".hh", ".hxx"]
        for header in headers:
            if name.endswith(header):
                return True
        return False

    @staticmethod
    def __cpp_file(name):
        """Return True if this a C++ header or source file."""
        file_exts = [".cpp", ".c", ".cxx", ".h", ".hpp", ".hh", ".hxx"]
        file_ext = os.path.splitext(name)[1]
        if file_ext in file_exts:
            return True
        return False

    def __write_filters(self):
        """Generate the vcxproj.filters file."""
        # 1. get a list of directories for all the files
        # 2. get all the C++ files in each of these dirs
        # 3. Output these lists of files to vcxproj and vcxproj.headers
        # Note: order of these lists does not matter, VS will sort them anyway
        dirs = set()
        bazel_files = set()

        for file_name in self.files:
            dirs.add(os.path.dirname(file_name))

        base_dirs = set()
        for directory in dirs:
            if not os.path.exists(directory):
                print(
                    (
                        "Warning: skipping include file scan for directory '%s'"
                        + " because it does not exist."
                    )
                    % str(directory)
                )
                continue

            # Get all the C++ files
            for file_name in os.listdir(directory):
                if self.__cpp_file(file_name):
                    self.files.add(directory + "\\" + file_name)

            # Make sure the set also includes the base directories
            # (i.e. src/mongo and src as examples)
            base_name = os.path.dirname(directory)
            while base_name:
                base_dirs.add(base_name)
                base_name = os.path.dirname(base_name)

        dirs = dirs.union(base_dirs)

        # Get all the bazel files
        for directory in dirs:
            if os.path.exists(directory):
                for file_name in os.listdir(directory):
                    if file_name == "BUILD.bazel" or ".bazel" in file_name:
                        bazel_files.add(directory + "\\" + file_name)

        # Write a list of directory entries with unique guids
        self.filters.write("  <ItemGroup>\n")
        for file_name in sorted(dirs):
            self.filters.write("    <Filter Include='%s'>\n" % file_name)
            self.filters.write("        <UniqueIdentifier>{%s}</UniqueIdentifier>\n" % uuid.uuid4())
            self.filters.write("    </Filter>\n")
        self.filters.write("  </ItemGroup>\n")

        # Write a list of files to compile
        self.filters.write("  <ItemGroup>\n")
        for file_name in sorted(self.files):
            if not self.__is_header(file_name):
                self.filters.write("    <ClCompile Include='%s'>\n" % file_name)
                self.filters.write("        <Filter>%s</Filter>\n" % os.path.dirname(file_name))
                self.filters.write("    </ClCompile>\n")
        self.filters.write("  </ItemGroup>\n")

        # Write a list of headers
        self.filters.write("  <ItemGroup>\n")
        for file_name in sorted(self.files):
            if self.__is_header(file_name):
                self.filters.write("    <ClInclude Include='%s'>\n" % file_name)
                self.filters.write("        <Filter>%s</Filter>\n" % os.path.dirname(file_name))
                self.filters.write("    </ClInclude>\n")
        self.filters.write("  </ItemGroup>\n")

        # Write a list of bazel files
        self.filters.write("  <ItemGroup>\n")
        for file_name in sorted(bazel_files):
            self.filters.write("    <None Include='%s'>\n" % file_name)
            self.filters.write("        <Filter>%s</Filter>\n" % os.path.dirname(file_name))
            self.filters.write("    </None>\n")
        self.filters.write("  </ItemGroup>\n")

        # Write a list of headers into the vcxproj
        self.vcxproj.write("  <ItemGroup>\n")
        for file_name in sorted(self.files):
            if self.__is_header(file_name):
                self.vcxproj.write("    <ClInclude Include='%s' />\n" % file_name)
        self.vcxproj.write("  </ItemGroup>\n")

        # Write a list of bazel files into the vcxproj
        self.vcxproj.write("  <ItemGroup>\n")
        for file_name in sorted(bazel_files):
            self.vcxproj.write("    <None Include='%s' />\n" % file_name)
        self.vcxproj.write("  </ItemGroup>\n")


def main():
    """Execute Main program."""
    parser = argparse.ArgumentParser(description="VS Project File Generator.")
    parser.add_argument(
        "--version",
        type=str,
        nargs="?",
        help="MSVC Toolchain version",
        default=VCXPROJ_MSVC_DEFAULT_VERSION,
    )
    parser.add_argument("target", type=str, help="File to generate")

    args = parser.parse_args()

    with ProjFileGenerator(args.target, args.version) as projfile:
        with open("compile_commands.json", "rb") as sjh:
            contents = sjh.read().decode("utf-8")
            commands = json.loads(contents)

        for command in commands:
            command_args = command["arguments"]
            projfile.parse_args(command_args)


main()
