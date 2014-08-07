# generate vcxproj file(s)
#
#   HOW TO USE
#
#   scons --clean
#   # verify build/* is empty...
#   scons TARGET.exe > out
#   python buildscripts/make_vcxproj.py TARGET < out > my.vcxproj
#
#   where TARGET is your target e.g., "mongod"
#
#   NOTES
#
#   (1)
#       directory paths are such that it is assumed the vcxproj is in the top level project directory.
#       this is easy and likely to change...
#
#   (2)
#       machine generated files (error_codes, action_types, ...) are, for now, copied by this script
#       into the source tree -- see note below in function pyth() as to why.
#       if those files need refreshing, run scons to generate them, and then run make_vcxproj.py again
#       to copy them over.  the rebuilding of the vcxproj file in that case should be moot, it is just
#       the copying over of the updated files we really want to happen.
#
#   (3)
#       todo: i don't think the generated vcxproj perfectly handles switching from debug to release and
#       such yet.  so for example:
#
#         scons --clean all && scons --dd --win2008plus --64 mongod.exe && python ...
#
#       should generate a file that will work for building mongod.exe, *if* you pick win2008plus and
#       Debug and 64 bit from the drop downs.  The other variations so far, ymmv.
#

import sys
import os

target = sys.argv[1]

footer= """
  </ItemGroup>

  <ItemGroup>
    <None Include="src\\mongo\\db\\mongo.ico" />
  </ItemGroup>

  <ItemGroup>
    <ResourceCompile Include="src\\mongo\\db\\db.rc" />
  </ItemGroup>

  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets"></ImportGroup>
</Project>
"""

common_defines_str = "/DBOOST_ALL_NO_LIB /DMONGO_EXPOSE_MACROS /DSUPPORT_UTF8 /D_UNICODE /DUNICODE /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0502 /DMONGO_HAVE___DECLSPEC_THREAD"

def get_defines(x):
    res = set()
    for s in x:
        if s.startswith('/D') or s.startswith('-D'):
            d = s[2:]
            res.add(d)
    return res

common_defines = get_defines(common_defines_str.split(' '))

f = open('buildscripts/vcxproj.header', 'r')
header = f.read().replace("%_TARGET_%", target)
print header

print "<!-- common_defines -->"
print "<ItemDefinitionGroup><ClCompile><PreprocessorDefinitions>"
print ';'.join(common_defines) + ";%(PreprocessorDefinitions)"
print "</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>\n"
print "<ItemGroup>\n"

# we don't use _SCONS in vcxproj files, but it's in the input to this script, so add it to common_defines
# so that it is ignored below and not declared:
common_defines.add("_SCONS")
# likewise we handle DEBUG and such in the vcxproj header:
common_defines.add("DEBUG")
common_defines.add("_DEBUG")
common_defines.add("V8_TARGET_ARCH_X64")
common_defines.add("V8_TARGET_ARCH_IA32")
common_defines.add("NTDDI_VERSION")
common_defines.add("_WIN32_WINNT")

machine_path = ""

def add_globally(path):
    print "\n</ItemGroup>\n"
    print "<ItemDefinitionGroup><ClCompile><AdditionalIncludeDirectories>" + machine_path + "</AdditionalIncludeDirectories></ClCompile></ItemDefinitionGroup>"
    print "<ItemGroup>\n"

def parse_line(x,line):
    # buildinfo.cpp is for scons only -- see version.cpp for more info
    if not "buildinfo.cpp" in x:
        xtra = ""
        if x.startswith('build\\'):
            parts = x.split(os.sep)
            if "mongo" in parts:
                x = os.sep.join(parts[parts.index("mongo"):])
            elif "third_party" in parts:
                x = os.sep.join(parts[parts.index("third_party"):])
            else:
                raise NameError("Bad input string for source file")
            x = "src" + os.sep + x
        if "v8\\src" in x: # or machine_path:
            xtra = "<AdditionalIncludeDirectories>"
            # it would be better to look at the command line inclusions comprehensively instead of hard code
            # this way, but this will get us going...
            if "v8\\src" in x:
                xtra += "src\\third_party\\v8\\src;"
            #if machine_path:
            #    xtra += machine_path
            xtra += "</AdditionalIncludeDirectories>"
        # add /D command line items that are uncommon
        defines = ""
        for s in get_defines(line):
            if s.split('=')[0] not in common_defines:
                defines += s
                defines += ';'
        if defines:
            xtra += "<PreprocessorDefinitions>" + defines + "%(PreprocessorDefinitions)</PreprocessorDefinitions>"
        if xtra != "":
            return "    <ClCompile Include=\"" + x + "\">" + xtra + "</ClCompile>"
        else:
            return "    <ClCompile Include=\"" + x + "\" />"

from shutil import copyfile

def main ():
    lines = set()
    for line in sys.stdin:
        x = line.split(' ')
        # Skip lines that use temp files as the argument to cl.exe
        if x[0] == "cl" and len(x) > 3:
            lines.add(parse_line(x[3],x))

    for line in lines:
        if line != None:
            print line

    print footer

main()
