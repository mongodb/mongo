# generate vcxproj file(s)
#
# howto:
#   scons --clean
#   scons <a_target> > out
#   python buildscripts/make_vcxproj.py < out > out1 && cat buildscripts/vcxproj.header out1 > a_target.vcxproj
#
# note: the vcxproj.header file currently references "mongod.exe" explicitly rather than being generic.
#       this will be changed soon but gets us started.  
#       also, directory paths are such that it is assumed the vcxproj is in the top level project directory.
#       this is easy and likely to change...
#

import sys
import os

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

common_defines_str = "/DBOOST_ALL_NO_LIB /DMONGO_EXPOSE_MACROS /DSUPPORT_UTF8 /D_UNICODE /DUNICODE /D_CONSOLE /D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0502 /DNTDDI_VERSION=0x05020200 /DMONGO_HAVE___DECLSPEC_THREAD"

def get_defines(x):
    res = set()
    for s in x:
        if s.startswith('/D') or s.startswith('-D'):
            d = s[2:]
            res.add(d)
    return res

common_defines = get_defines(common_defines_str.split(' '))

print "\n<ItemDefinitionGroup><ClCompile><PreprocessorDefinitions>"
print ';'.join(common_defines) + ";%(PreprocessorDefinitions)"
print "</PreprocessorDefinitions></ClCompile></ItemDefinitionGroup>\n"
print "<ItemGroup>\n"

# we don't use _SCONS in vcxproj files, but it's in the input to this script, so add it to common_defines:
common_defines.add("_SCONS")
# likewise we handle DEBUG and such in the vcxproj header:
common_defines.add("DEBUG")
common_defines.add("_DEBUG")
common_defines.add("V8_TARGET_ARCH_X64")
common_defines.add("V8_TARGET_ARCH_IA32")

def say(x,line):
    # buildinfo.cpp is for scons only -- see version.cpp for more info
    if not "buildinfo.cpp" in x:
        xtra = ""
        # it would be better to look at the command line inclusions comprehensively instead of hard code
        # this way, but this will get us going...
        if "v8\\src" in x:
            xtra = "<AdditionalIncludeDirectories>src\\third_party\\v8\\src</AdditionalIncludeDirectories>"
        # add /D command line items that are uncommon
        defines = ""
        for s in get_defines(line):
            if s not in common_defines:
                defines += s
                defines += ';'
        if defines:
            xtra += "<PreprocessorDefinitions>" + defines + "%(PreprocessorDefinitions)</PreprocessorDefinitions>"
        print "    <ClCompile Include=\"" + x + "\">" + xtra + "</ClCompile>"

def main ():
    for line in sys.stdin:
        x = line.split(' ')
        if x[0] == "cl":
            say(x[3],x)
    print footer

main()
