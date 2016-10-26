# Generate vcxproj and vcxproj.filters files for browsing code in Visual Studio 2015.
# To build mongodb, you must use scons. You can use this project to navigate code during debugging.
#
#  HOW TO USE
#
#  First, you need a compile_commands.json file, to generate run the following command:
#    scons compiledb
#
#  Next, run the following command
#    python buildscripts/make_vcxproj.py FILE_NAME
#
#   where FILE_NAME is the of the file to generate e.g., "mongod"
#

import json
import os
import re
import sys
import uuid

VCXPROJ_FOOTER = r"""

  <ItemGroup>
    <None Include="src\mongo\db\mongo.ico" />
  </ItemGroup>

  <ItemGroup>
    <ResourceCompile Include="src\mongo\db\db.rc" />
  </ItemGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets"></ImportGroup>
</Project>
"""

def get_defines(args):
    """Parse a compiler argument list looking for defines"""
    ret = set()
    for arg in args:
        if arg.startswith('/D'):
            ret.add(arg[2:])
    return ret

def get_includes(args):
    """Parse a compiler argument list  looking for includes"""
    ret = set()
    for arg in args:
        if arg.startswith('/I'):
            ret.add(arg[2:])
    return ret

class ProjFileGenerator(object):
    """Generate a .vcxproj and .vcxprof.filters file"""
    def __init__(self, target):
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

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.vcxproj = open(self.target + ".vcxproj", "wb")

        with open('buildscripts/vcxproj.header', 'r') as header_file:
            header_str = header_file.read()
            header_str = header_str.replace("%_TARGET_%", self.target)
            header_str = header_str.replace("%AdditionalIncludeDirectories%",
                                            ';'.join(sorted(self.includes)))
            self.vcxproj.write(header_str)

        common_defines = self.all_defines
        for c in self.compiles:
            common_defines = common_defines.intersection(c['defines'])

        self.vcxproj.write("<!-- common_defines -->\n")
        self.vcxproj.write("<ItemDefinitionGroup><ClCompile><PreprocessorDefinitions>"
                           + ';'.join(common_defines) + ";%(PreprocessorDefinitions)\n")
        self.vcxproj.write("</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>\n")

        self.vcxproj.write("  <ItemGroup>\n")
        for command in self.compiles:
            defines = command["defines"].difference(common_defines)
            if len(defines) > 0:
                self.vcxproj.write("    <ClCompile Include=\"" + command["file"] +
                                   "\"><PreprocessorDefinitions>" +
                                   ';'.join(defines) +
                                   ";%(PreprocessorDefinitions)" +
                                   "</PreprocessorDefinitions></ClCompile>\n")
            else:
                self.vcxproj.write("    <ClCompile Include=\"" + command["file"] + "\" />\n")
        self.vcxproj.write("  </ItemGroup>\n")

        self.filters = open(self.target + ".vcxproj.filters", "wb")
        self.filters.write("<?xml version='1.0' encoding='utf-8'?>\n")
        self.filters.write("<Project ToolsVersion='14.0' " +
                           "xmlns='http://schemas.microsoft.com/developer/msbuild/2003'>\n")

        self.__write_filters()

        self.vcxproj.write(VCXPROJ_FOOTER)
        self.vcxproj.close()

        self.filters.write("</Project>\n")
        self.filters.close()

    def parse_line(self, line):
        """Parse a build line"""
        if line.startswith("cl"):
            self.__parse_cl_line(line[3:])

    def __parse_cl_line(self, line):
        """Parse a compiler line"""
        # Get the file we are compilong
        file_name = re.search(r"/c ([\w\\.-]+) ", line).group(1)

        # Skip files made by scons for configure testing
        if "sconf_temp" in file_name:
            return

        self.files.add(file_name)

        args = line.split(' ')

        file_defines = set()
        for arg in get_defines(args):
            if arg not in self.common_defines:
                file_defines.add(arg)
        self.all_defines = self.all_defines.union(file_defines)

        for arg in get_includes(args):
            self.includes.add(arg)

        self.compiles.append({"file" : file_name, "defines" : file_defines})

    def __is_header(self, name):
        """Is this a header file?"""
        headers = [".h", ".hpp", ".hh", ".hxx"]
        for header in headers:
            if name.endswith(header):
                return True
        return False

    def __write_filters(self):
        """Generate the vcxproj.filters file"""
        # 1. get a list of directories for all the files
        # 2. get all the headers in each of these dirs
        # 3. Output these lists of files to vcxproj and vcxproj.headers
        # Note: order of these lists does not matter, VS will sort them anyway
        dirs = set()
        scons_files = set()

        for file_name in self.files:
            dirs.add(os.path.dirname(file_name))

        base_dirs = set()
        for directory in dirs:
            if not os.path.exists(directory):
                print(("Warning: skipping include file scan for directory '%s'" +
                      " because it does not exist.") % str(directory))
                continue

            # Get all the header files
            for file_name in os.listdir(directory):
                if self.__is_header(file_name):
                    self.files.add(directory + "\\" + file_name)

            # Make sure the set also includes the base directories
            # (i.e. src/mongo and src as examples)
            base_name = os.path.dirname(directory)
            while base_name:
                base_dirs.add(base_name)
                base_name = os.path.dirname(base_name)

        dirs = dirs.union(base_dirs)

        # Get all the scons files
        for directory in dirs:
            if os.path.exists(directory):
                for file_name in os.listdir(directory):
                    if "SConstruct" == file_name or "SConscript" in file_name:
                        scons_files.add(directory + "\\" + file_name)
        scons_files.add("SConstruct")

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

        # Write a list of scons files
        self.filters.write("  <ItemGroup>\n")
        for file_name in sorted(scons_files):
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

        # Write a list of scons files into the vcxproj
        self.vcxproj.write("  <ItemGroup>\n")
        for file_name in sorted(scons_files):
            self.vcxproj.write("    <None Include='%s' />\n" % file_name)
        self.vcxproj.write("  </ItemGroup>\n")

def main():
    if len(sys.argv) != 2:
        print r"Usage: python buildscripts\make_vcxproj.py FILE_NAME"
        return

    with ProjFileGenerator(sys.argv[1]) as projfile:
        with open("compile_commands.json", "rb") as sjh:
            contents = sjh.read().decode('utf-8')
            commands = json.loads(contents)

        for command in commands:
            command_str = command["command"]
            projfile.parse_line(command_str)

main()
