# generate vcxproj file(s)
#
# howto:
#   scons --clean
#   scons <a_target> > out
#   python buildscripts/make_vcxproj.py TARGET < out > my.vcxproj
#
#   where TARGET is your target e.g., "mongod"
#
#   note:
#       directory paths are such that it is assumed the vcxproj is in the top level project directory.
#       this is easy and likely to change...
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
header = f.read().replace("_TARGET_", target)
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

def say(x,line):
    # buildinfo.cpp is for scons only -- see version.cpp for more info
    if not "buildinfo.cpp" in x:
        if x.startswith('build\\'):
            #global machine_path
            #if not machine_path:
            #    machine_path = x.split("mongo")[0]
            #sys.stderr.write("todo: adding machine gen'd include path " + machine_path + " to vcxproj\n")
            sys.stderr.write("adding machine gen'd file " + x + " to vcxproj\n")
        xtra = ""
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
        print "    <ClCompile Include=\"" + x + "\">" + xtra + "</ClCompile>"

from shutil import copyfile

# for the machine generated files we copy them into the src build tree locally.
# this is annoying but vstudio doesn't seem to like having parallel sets of -I include
# paths so had to do this to make it happy
def pyth(x):
    for s in x:
        if s.startswith("build") and s.endswith(".h"):
            sys.stderr.write("copying " + s + " to src/ tree\n")
            copyfile(s, 'src\mongo' + s.split("mongo")[1])

def main ():
    for line in sys.stdin:
        x = line.split(' ')
        if x[0] == "cl":
            say(x[3],x)
        elif "python" in x[0]:
            pyth(x)
    print footer

main()
