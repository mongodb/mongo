
This list was created with:

```
curl https://api.github.com/search/issues?q="repo%3Ajson-c%2Fjson-c+closed%3A>2014-04-10+created%3A<2017-12-01&sort=created&order=asc&per_page=400&page=1" > issues1.out
curl https://api.github.com/search/issues?q="repo%3Ajson-c%2Fjson-c+closed%3A>2014-04-10+created%3A<2017-12-01&sort=created&order=asc&per_page=400&page=2" > issues2.out
curl https://api.github.com/search/issues?q="repo%3Ajson-c%2Fjson-c+closed%3A>2014-04-10+created%3A<2017-12-01&sort=created&order=asc&per_page=400&page=3" > issues3.out
jq -r '.items[] | "[" + .title + "](" + .url + ")" | tostring' issues?.out  > issues.md
sed -e's,^\[ *\(.*\)\](https://api.github.com/.*/\([0-9].*\)),[Issue #\2](https://github.com/json-c/json-c/issues/\2) - \1,' -i issues.md
#... manual editing ...
```

----

Issues and Pull Requests closed for the 0.13 release
(since commit f84d9c, the 0.12 branch point, 2014-04-10)


* [Issue #61](https://github.com/json-c/json-c/issues/61) - Make json_object_object_add() indicate success or failure, test fix \
* [Issue #113](https://github.com/json-c/json-c/issues/113) - Build fixes (make dist and make distcheck) \
* [Issue #124](https://github.com/json-c/json-c/issues/124) - Fixing build \
* [Issue #125](https://github.com/json-c/json-c/issues/125) - Fix compile error(variable size set but not used) on g++4.6 \
* [Issue #126](https://github.com/json-c/json-c/issues/126) - Removed unused size variable. \
* [Issue #127](https://github.com/json-c/json-c/issues/127) - remove unused `size` variable \
* [Issue #128](https://github.com/json-c/json-c/issues/128) - Remove unused variable from json_tokenizer.c \
* [Issue #130](https://github.com/json-c/json-c/issues/130) - Failed to compile under Ubuntu 13.10 32bit \
* [Issue #131](https://github.com/json-c/json-c/issues/131) - undefined symbol: __sync_val_compare_and_swap_4 \
* [Issue #132](https://github.com/json-c/json-c/issues/132) - Remove unused variable 'size' \
* [Issue #133](https://github.com/json-c/json-c/issues/133) - Update and rename README to README.md \
* [Issue #134](https://github.com/json-c/json-c/issues/134) - Must remove variable size... \
* [Issue #135](https://github.com/json-c/json-c/issues/135) - bits.h uses removed json_tokener_errors\[error\] \
* [Issue #136](https://github.com/json-c/json-c/issues/136) - Error when running make check \
* [Issue #137](https://github.com/json-c/json-c/issues/137) - config.h.in should not be in git \
* [Issue #138](https://github.com/json-c/json-c/issues/138) - Can't build on RHEL 6.5 due to dependency on automake-1.14 \
* [Issue #140](https://github.com/json-c/json-c/issues/140) - Code bug in random_test.c evaluating same expression twice \
* [Issue #141](https://github.com/json-c/json-c/issues/141) - Removed duplicate check in random_seed test - bug #140 \
* [Issue #142](https://github.com/json-c/json-c/issues/142) - Please undeprecate json_object_object_get \
* [Issue #144](https://github.com/json-c/json-c/issues/144) - Introduce json_object_from_fd \
* [Issue #145](https://github.com/json-c/json-c/issues/145) - Handle % character properly \
* [Issue #146](https://github.com/json-c/json-c/issues/146) - TAGS rename \
* [Issue #148](https://github.com/json-c/json-c/issues/148) - Bump the soname \
* [Issue #149](https://github.com/json-c/json-c/issues/149) - SONAME bump \
* [Issue #150](https://github.com/json-c/json-c/issues/150) - Fix build using MinGW. \
* [Issue #151](https://github.com/json-c/json-c/issues/151) - Remove json_type enum trailing comma \
* [Issue #152](https://github.com/json-c/json-c/issues/152) - error while compiling json-c library version 0.11 \
* [Issue #153](https://github.com/json-c/json-c/issues/153) - improve doc for json_object_to_json_string() \
* [Issue #154](https://github.com/json-c/json-c/issues/154) - double precision \
* [Issue #155](https://github.com/json-c/json-c/issues/155) - add bsearch for arrays \
* [Issue #156](https://github.com/json-c/json-c/issues/156) - Remove trailing whitespaces \
* [Issue #157](https://github.com/json-c/json-c/issues/157) - JSON-C shall not exit on calloc fail. \
* [Issue #158](https://github.com/json-c/json-c/issues/158) - while using json-c 0.11, I am facing strange crash issue in json_object_put. \
* [Issue #159](https://github.com/json-c/json-c/issues/159) - json_tokener.c compile error \
* [Issue #160](https://github.com/json-c/json-c/issues/160) - missing header file on windows?? \
* [Issue #161](https://github.com/json-c/json-c/issues/161) - Is there a way to append to file? \
* [Issue #162](https://github.com/json-c/json-c/issues/162) - json_util: add directory check for POSIX distros \
* [Issue #163](https://github.com/json-c/json-c/issues/163) - Fix Win32 build problems \
* [Issue #164](https://github.com/json-c/json-c/issues/164) - made it compile and link on Widnows (as static library) \
* [Issue #165](https://github.com/json-c/json-c/issues/165) - json_object_to_json_string_ext length \
* [Issue #167](https://github.com/json-c/json-c/issues/167) - Can't build on Windows with Visual Studio 2010 \
* [Issue #168](https://github.com/json-c/json-c/issues/168) - Tightening the number parsing algorithm \
* [Issue #169](https://github.com/json-c/json-c/issues/169) - Doesn't compile on ubuntu 14.04, 64bit \
* [Issue #170](https://github.com/json-c/json-c/issues/170) - Generated files in repository \
* [Issue #171](https://github.com/json-c/json-c/issues/171) - Update configuration for VS2010 and win64 \
* [Issue #172](https://github.com/json-c/json-c/issues/172) - Adding support for parsing octal numbers \
* [Issue #173](https://github.com/json-c/json-c/issues/173) - json_parse_int64 doesn't work correctly at illumos \
* [Issue #174](https://github.com/json-c/json-c/issues/174) - Adding JSON_C_TO_STRING_PRETTY_TAB flag \
* [Issue #175](https://github.com/json-c/json-c/issues/175) - make check fails 4 tests with overflows when built with ASAN \
* [Issue #176](https://github.com/json-c/json-c/issues/176) - Possible to delete an array element at a given idx ? \
* [Issue #177](https://github.com/json-c/json-c/issues/177) - Fix compiler warnings \
* [Issue #178](https://github.com/json-c/json-c/issues/178) - Unable to compile on CentOS5 \
* [Issue #179](https://github.com/json-c/json-c/issues/179) - Added array_list_del_idx and json_object_array_del_idx \
* [Issue #180](https://github.com/json-c/json-c/issues/180) - Enable silent build by default \
* [Issue #181](https://github.com/json-c/json-c/issues/181) - json_tokener_parse_ex accepts invalid JSON \
* [Issue #182](https://github.com/json-c/json-c/issues/182) - Link against libm when needed \
* [Issue #183](https://github.com/json-c/json-c/issues/183) - Apply compile warning fix to master branch \
* [Issue #184](https://github.com/json-c/json-c/issues/184) - Use only GCC-specific flags when compiling with GCC \
* [Issue #185](https://github.com/json-c/json-c/issues/185) - compile error \
* [Issue #186](https://github.com/json-c/json-c/issues/186) - Syntax error \
* [Issue #187](https://github.com/json-c/json-c/issues/187) - array_list_get_idx and negative indexes. \
* [Issue #188](https://github.com/json-c/json-c/issues/188) - json_object_object_foreach warnings \
* [Issue #189](https://github.com/json-c/json-c/issues/189) - noisy json_object_from_file: error opening file \
* [Issue #190](https://github.com/json-c/json-c/issues/190) - warning: initialization discards const qualifier from pointer target type \[enabled by default\] \
* [Issue #192](https://github.com/json-c/json-c/issues/192) - json_tokener_parse  accepts invalid JSON {"key": "value" ,  } \
* [Issue #193](https://github.com/json-c/json-c/issues/193) - Make serialization format of doubles configurable \
* [Issue #194](https://github.com/json-c/json-c/issues/194) - Add utility function for comparing json_objects \
* [Issue #195](https://github.com/json-c/json-c/issues/195) - Call uselocale instead of setlocale \
* [Issue #196](https://github.com/json-c/json-c/issues/196) - Performance improvements \
* [Issue #197](https://github.com/json-c/json-c/issues/197) - Time for a new release? \
* [Issue #198](https://github.com/json-c/json-c/issues/198) - Fix possible memory leak and remove superfluous NULL checks before free() \
* [Issue #199](https://github.com/json-c/json-c/issues/199) - Fix build in Visual Studio \
* [Issue #200](https://github.com/json-c/json-c/issues/200) - Add build scripts for CI platforms \
* [Issue #201](https://github.com/json-c/json-c/issues/201) - disable forward-slash escaping? \
* [Issue #202](https://github.com/json-c/json-c/issues/202) - Array with objects support \
* [Issue #203](https://github.com/json-c/json-c/issues/203) - Add source position/coordinates to API \
* [Issue #204](https://github.com/json-c/json-c/issues/204) - json-c/json.h not found \
* [Issue #205](https://github.com/json-c/json-c/issues/205) - json-c Compiled with Visual Studios \
* [Issue #206](https://github.com/json-c/json-c/issues/206) - what do i use in place of json_object_object_get? \
* [Issue #207](https://github.com/json-c/json-c/issues/207) - Add support for property pairs directly added to arrays \
* [Issue #208](https://github.com/json-c/json-c/issues/208) - Performance enhancements (mainly) to json_object_to_json_string() \
* [Issue #209](https://github.com/json-c/json-c/issues/209) - fix regression from 2d549662be832da838aa063da2efa78ee3b99668 \
* [Issue #210](https://github.com/json-c/json-c/issues/210) - Use size_t for arrays \
* [Issue #211](https://github.com/json-c/json-c/issues/211) - Atomic updates for the refcount \
* [Issue #212](https://github.com/json-c/json-c/issues/212) - Refcount doesn't work between threads \
* [Issue #213](https://github.com/json-c/json-c/issues/213) - fix to compile with microsoft visual c++ 2010 \
* [Issue #214](https://github.com/json-c/json-c/issues/214) - Some non-GNU systems support __sync_val_compare_and_swap \
* [Issue #215](https://github.com/json-c/json-c/issues/215) - Build json-c for window 64 bit. \
* [Issue #216](https://github.com/json-c/json-c/issues/216) - configure: check realloc with AC_CHECK_FUNCS() to fix cross-compilation. \
* [Issue #217](https://github.com/json-c/json-c/issues/217) - Checking for functions in float.h \
* [Issue #218](https://github.com/json-c/json-c/issues/218) - Use a macro to indicate C99 to the compiler \
* [Issue #219](https://github.com/json-c/json-c/issues/219) - Fix various potential null ptr deref and int32 overflows \
* [Issue #220](https://github.com/json-c/json-c/issues/220) - Add utility function for comparing json_objects \
* [Issue #221](https://github.com/json-c/json-c/issues/221) - JSON_C_TO_STRING_NOSLASHESCAPE works incorrectly \
* [Issue #222](https://github.com/json-c/json-c/issues/222) - Fix issue #221: JSON_C_TO_STRING_NOSLASHESCAPE works incorrectly \
* [Issue #223](https://github.com/json-c/json-c/issues/223) - Clarify json_object_get_string documentation of NULL handling & return \
* [Issue #224](https://github.com/json-c/json-c/issues/224) - json_tokener.c - all warnings being treated as errors \
* [Issue #225](https://github.com/json-c/json-c/issues/225) - Hi, will you support clib as a "registry"? \
* [Issue #227](https://github.com/json-c/json-c/issues/227) - Bump SOVERSION to 3 \
* [Issue #228](https://github.com/json-c/json-c/issues/228) - avoid double slashes from json \
* [Issue #229](https://github.com/json-c/json-c/issues/229) - configure fails: checking size of size_t... configure: error: cannot determine a size for size_t \
* [Issue #230](https://github.com/json-c/json-c/issues/230) - Use stdint.h to check for size_t size \
* [Issue #231](https://github.com/json-c/json-c/issues/231) - Fix size_t size check for first-time builds \
* [Issue #232](https://github.com/json-c/json-c/issues/232) - tests/tests1: fix printf format for size_t arguments \
* [Issue #233](https://github.com/json-c/json-c/issues/233) - Include stddef.h in json_object.h \
* [Issue #234](https://github.com/json-c/json-c/issues/234) - Add public API to use userdata independently of custom serializer \
* [Issue #235](https://github.com/json-c/json-c/issues/235) - Undefined symbols Error for architecture x86_64 on Mac \
* [Issue #236](https://github.com/json-c/json-c/issues/236) - Building a project which uses json-c with flag -Wcast-qual causes compilation errors \
* [Issue #237](https://github.com/json-c/json-c/issues/237) - handle escaped utf-8 \
* [Issue #238](https://github.com/json-c/json-c/issues/238) - linkhash.c: optimised the table_free path \
* [Issue #239](https://github.com/json-c/json-c/issues/239) - initialize null terminator of new printbuf \
* [Issue #240](https://github.com/json-c/json-c/issues/240) - Compile error: Variable set but not used \
* [Issue #241](https://github.com/json-c/json-c/issues/241) - getting error in date string 19\/07\/2016, fixed for error 19/07/2016 \
* [Issue #242](https://github.com/json-c/json-c/issues/242) - json_tokener_parse error \
* [Issue #243](https://github.com/json-c/json-c/issues/243) - Fix #165 \
* [Issue #244](https://github.com/json-c/json-c/issues/244) - Error while compiling source from RHEL5, could you please help me to fix this \
* [Issue #245](https://github.com/json-c/json-c/issues/245) - json-c compile in window xp \
* [Issue #246](https://github.com/json-c/json-c/issues/246) - Mac: uselocale failed to build \
* [Issue #247](https://github.com/json-c/json-c/issues/247) - json_object_array_del_idx function has segment fault error? \
* [Issue #248](https://github.com/json-c/json-c/issues/248) - Minor changes in C source code \
* [Issue #249](https://github.com/json-c/json-c/issues/249) - Improving README \
* [Issue #250](https://github.com/json-c/json-c/issues/250) - Improving .gitignore \
* [Issue #251](https://github.com/json-c/json-c/issues/251) - Adding a file for EditorConfig \
* [Issue #252](https://github.com/json-c/json-c/issues/252) - Very minor changes not related to C source code \
* [Issue #253](https://github.com/json-c/json-c/issues/253) - Adding a test with cppcheck for Travis CI \
* [Issue #254](https://github.com/json-c/json-c/issues/254) - Very minor changes to some tests \
* [Issue #255](https://github.com/json-c/json-c/issues/255) - Minor changes in C source code \
* [Issue #256](https://github.com/json-c/json-c/issues/256) - Mailing list dead? \
* [Issue #257](https://github.com/json-c/json-c/issues/257) - Defining a coding style \
* [Issue #258](https://github.com/json-c/json-c/issues/258) - Enable CI services \
* [Issue #259](https://github.com/json-c/json-c/issues/259) - Fails to parse valid json \
* [Issue #260](https://github.com/json-c/json-c/issues/260) - Adding an object to itself \
* [Issue #261](https://github.com/json-c/json-c/issues/261) - Lack of proper documentation \
* [Issue #262](https://github.com/json-c/json-c/issues/262) - Add Cmakefile and fix compiler warning. \
* [Issue #263](https://github.com/json-c/json-c/issues/263) - Compiler Warnings with VS2015 \
* [Issue #264](https://github.com/json-c/json-c/issues/264) - successed in simple test   while failed in my project \
* [Issue #265](https://github.com/json-c/json-c/issues/265) - Conformance report for reference \
* [Issue #266](https://github.com/json-c/json-c/issues/266) - crash perhaps related to reference counting \
* [Issue #267](https://github.com/json-c/json-c/issues/267) - Removes me as Win32 maintainer, because I'm not. \
* [Issue #268](https://github.com/json-c/json-c/issues/268) - Documentation of json_object_to_json_string gives no information about memory management \
* [Issue #269](https://github.com/json-c/json-c/issues/269) - json_object_<type>_set(json_object *o,<type> value) API for value setting in json object private structure \
* [Issue #270](https://github.com/json-c/json-c/issues/270) - new API json_object_new_double_f(doubel d,const char * fmt); \
* [Issue #271](https://github.com/json-c/json-c/issues/271) - Cannot compile using CMake on macOS \
* [Issue #273](https://github.com/json-c/json-c/issues/273) - fixed wrong object name in json_object_all_values_equal \
* [Issue #274](https://github.com/json-c/json-c/issues/274) - Support for 64 bit pointers on Windows \
* [Issue #275](https://github.com/json-c/json-c/issues/275) - Out-of-bounds read in json_tokener_parse_ex \
* [Issue #276](https://github.com/json-c/json-c/issues/276) - ./configure for centos release 6.7(final) failure \
* [Issue #277](https://github.com/json-c/json-c/issues/277) - Json object set xxx \
* [Issue #278](https://github.com/json-c/json-c/issues/278) - Serialization of double with no fractional component drops trailing zero \
* [Issue #279](https://github.com/json-c/json-c/issues/279) - Segmentation fault in array_list_length() \
* [Issue #280](https://github.com/json-c/json-c/issues/280) - Should json_object_array_get_idx  check whether input obj is array? \
* [Issue #281](https://github.com/json-c/json-c/issues/281) - how to pretty print json-c? \
* [Issue #282](https://github.com/json-c/json-c/issues/282) - ignore temporary files \
* [Issue #283](https://github.com/json-c/json-c/issues/283) - json_pointer: add first revision based on RFC 6901 \
* [Issue #284](https://github.com/json-c/json-c/issues/284) - Resusing  json_tokener object \
* [Issue #285](https://github.com/json-c/json-c/issues/285) - Revert "compat/strdup.h: move common compat check for strdup() to own \
* [Issue #286](https://github.com/json-c/json-c/issues/286) - json_tokener_parse_ex() returns json_tokener_continue on zero-length string \
* [Issue #287](https://github.com/json-c/json-c/issues/287) - json_pointer: extend setter & getter with printf() style arguments \
* [Issue #288](https://github.com/json-c/json-c/issues/288) - Fix _GNU_SOURCE define for vasprintf \
* [Issue #289](https://github.com/json-c/json-c/issues/289) - bugfix: floating point representaion without fractional part \
* [Issue #290](https://github.com/json-c/json-c/issues/290) - duplicate an json_object \
* [Issue #291](https://github.com/json-c/json-c/issues/291) - isspace assert error \
* [Issue #292](https://github.com/json-c/json-c/issues/292) - configure error  "./configure: line 13121: syntax error near unexpected token `-Wall'" \
* [Issue #293](https://github.com/json-c/json-c/issues/293) - how to make with bitcode for ios \
* [Issue #294](https://github.com/json-c/json-c/issues/294) - Adding UTF-8 validation.  Fixes #122 \
* [Issue #295](https://github.com/json-c/json-c/issues/295) - cross compile w/ mingw \
* [Issue #296](https://github.com/json-c/json-c/issues/296) - Missing functions header in json_object.h \
* [Issue #297](https://github.com/json-c/json-c/issues/297) - could not parse string to Json object? Like string str=\"helloworld;E\\test\\log\\;end\" \
* [Issue #298](https://github.com/json-c/json-c/issues/298) - Building using CMake doesn't work \
* [Issue #299](https://github.com/json-c/json-c/issues/299) - Improve json_object -> string performance \
* [Issue #300](https://github.com/json-c/json-c/issues/300) - Running tests with MinGW build \
* [Issue #301](https://github.com/json-c/json-c/issues/301) - How to deep copy  json_object in C++ ? \
* [Issue #302](https://github.com/json-c/json-c/issues/302) - json_tokener_parse_ex doesn't parse JSON values \
* [Issue #303](https://github.com/json-c/json-c/issues/303) - fix doc in tokener header file \
* [Issue #304](https://github.com/json-c/json-c/issues/304) - (.text+0x72846): undefined reference to `is_error' \
* [Issue #305](https://github.com/json-c/json-c/issues/305) - Fix compilation without C-99 option \
* [Issue #306](https://github.com/json-c/json-c/issues/306) - ./configure: line 12748 -error=deprecated-declarations \
* [Issue #307](https://github.com/json-c/json-c/issues/307) - Memory leak in json_tokener_parse \
* [Issue #308](https://github.com/json-c/json-c/issues/308) - AM_PROG_LIBTOOL not found on Linux \
* [Issue #309](https://github.com/json-c/json-c/issues/309) - GCC 7 reports various -Wimplicit-fallthrough= errors \
* [Issue #310](https://github.com/json-c/json-c/issues/310) - Add FALLTHRU comment to handle GCC7 warnings. \
* [Issue #311](https://github.com/json-c/json-c/issues/311) - Fix error C3688 when compiling on Visual Studio 2015 \
* [Issue #312](https://github.com/json-c/json-c/issues/312) - Fix CMake Build process improved for MinGW and MSYS2 \
* [Issue #313](https://github.com/json-c/json-c/issues/313) - VERBOSE=1 make check; tests/test_util_file.test.c and tests/test_util_file.expected out of sync \
* [Issue #315](https://github.com/json-c/json-c/issues/315) - Passing -1 to json_tokener_parse_ex is possibly unsafe \
* [Issue #316](https://github.com/json-c/json-c/issues/316) - Memory Returned by json_object_to_json_string not freed \
* [Issue #317](https://github.com/json-c/json-c/issues/317) - json_object_get_string gives segmentation error \
* [Issue #318](https://github.com/json-c/json-c/issues/318) - PVS-Studio static analyzer analyze results \
* [Issue #319](https://github.com/json-c/json-c/issues/319) - Windows: Fix dynamic library build with Visual Studio \
* [Issue #320](https://github.com/json-c/json-c/issues/320) - Can't compile in Mac OS X El Capitan \
* [Issue #321](https://github.com/json-c/json-c/issues/321) - build,cmake: fix vasprintf implicit definition and generate both static & shared libs \
* [Issue #322](https://github.com/json-c/json-c/issues/322) - can not link with libjson-c.a \
* [Issue #323](https://github.com/json-c/json-c/issues/323) - implicit fallthrough detected by gcc 7.1 \
* [Issue #324](https://github.com/json-c/json-c/issues/324) - JsonPath like function? \
* [Issue #325](https://github.com/json-c/json-c/issues/325) - Fix stack buffer overflow in json_object_double_to_json_string_format() \
* [Issue #327](https://github.com/json-c/json-c/issues/327) - why json-c so hard to compile \
* [Issue #328](https://github.com/json-c/json-c/issues/328) - json_object: implement json_object_deep_copy() function \
* [Issue #329](https://github.com/json-c/json-c/issues/329) - build,cmake: build,cmake: rename libjson-c-static.a to libjson-c.a \
* [Issue #330](https://github.com/json-c/json-c/issues/330) - tests: symlink basic tests to a single file that has the common code \
* [Issue #331](https://github.com/json-c/json-c/issues/331) - Safe use of snprintf() / vsnprintf() for Visual studio, and thread-safety fix \
* [Issue #332](https://github.com/json-c/json-c/issues/332) - Valgrind: invalid read after json_object_array_del_idx. \
* [Issue #333](https://github.com/json-c/json-c/issues/333) - Replace obsolete AM_PROG_LIBTOOL \
* [Issue #335](https://github.com/json-c/json-c/issues/335) - README.md: show build status tag from travis-ci.org \
* [Issue #336](https://github.com/json-c/json-c/issues/336) - tests: fix tests in travis-ci.org \
* [Issue #337](https://github.com/json-c/json-c/issues/337) - Synchronize "potentially racy" random seed in lh_char_hash() \
* [Issue #338](https://github.com/json-c/json-c/issues/338) - implement json_object_int_inc(json_object *, int64_t) \
* [Issue #339](https://github.com/json-c/json-c/issues/339) - Json schema validation \
* [Issue #340](https://github.com/json-c/json-c/issues/340) - strerror_override: add extern "C" and JSON_EXPORT specifiers for Visual C++ compilers \
* [Issue #341](https://github.com/json-c/json-c/issues/341) - character "/" parse as "\/" \
* [Issue #342](https://github.com/json-c/json-c/issues/342) - No such file or directory "/usr/include/json.h" \
* [Issue #343](https://github.com/json-c/json-c/issues/343) - Can't parse json \
* [Issue #344](https://github.com/json-c/json-c/issues/344) - Fix Mingw build \
* [Issue #345](https://github.com/json-c/json-c/issues/345) - Fix make dist and make distcheck \
* [Issue #346](https://github.com/json-c/json-c/issues/346) - Clamp double to int32 when narrowing in json_object_get_int. \
* [Issue #347](https://github.com/json-c/json-c/issues/347) - MSVC linker error json_c_strerror \
* [Issue #348](https://github.com/json-c/json-c/issues/348) - why \
* [Issue #349](https://github.com/json-c/json-c/issues/349) - `missing` is missing? \
* [Issue #350](https://github.com/json-c/json-c/issues/350) - stderror-override and disable-shared \
* [Issue #351](https://github.com/json-c/json-c/issues/351) - SIZE_T_MAX redefined from limits.h \
* [Issue #352](https://github.com/json-c/json-c/issues/352) - `INSTALL` overrides an automake script. \
* [Issue #353](https://github.com/json-c/json-c/issues/353) - Documentation issues \
* [Issue #354](https://github.com/json-c/json-c/issues/354) - Fixes #351 #352 #353 \
* [Issue #355](https://github.com/json-c/json-c/issues/355) - 1.make it can been compiled with Visual Studio 2010 by modify the CMakeList.txt and others \
* [Issue #356](https://github.com/json-c/json-c/issues/356) - VS2008 test  test_util_file.cpp err! \
* [Issue #357](https://github.com/json-c/json-c/issues/357) - __json_c_strerror incompatibility with link-time optimization \
* [Issue #358](https://github.com/json-c/json-c/issues/358) - make issue \
* [Issue #359](https://github.com/json-c/json-c/issues/359) - update CMakeLists.txt for compile with visual studio at least 2010 \
* [Issue #360](https://github.com/json-c/json-c/issues/360) - Use strtoll() to parse ints \
* [Issue #361](https://github.com/json-c/json-c/issues/361) - Fix double to int cast overflow in json_object_get_int64. \
* [Issue #362](https://github.com/json-c/json-c/issues/362) - CMake Package Config \
* [Issue #363](https://github.com/json-c/json-c/issues/363) - Issue #338, add json_object_add_int functions \
* [Issue #364](https://github.com/json-c/json-c/issues/364) - Cmake is Errir \
* [Issue #365](https://github.com/json-c/json-c/issues/365) - added fallthrough for gcc7 \
* [Issue #366](https://github.com/json-c/json-c/issues/366) - how to check  the json string,crash! \
* [Issue #367](https://github.com/json-c/json-c/issues/367) - Is json-c support "redirect" semantic? \
* [Issue #368](https://github.com/json-c/json-c/issues/368) - Add examples \
* [Issue #369](https://github.com/json-c/json-c/issues/369) - How to build json-c library for android? \
* [Issue #370](https://github.com/json-c/json-c/issues/370) - Compiling using clang-cl \
* [Issue #371](https://github.com/json-c/json-c/issues/371) - Invalid parsing for Infinity with json-c 0.12 \
* [Issue #372](https://github.com/json-c/json-c/issues/372) - Json-c 0.12: Fixed Infinity bug \
* [Issue #373](https://github.com/json-c/json-c/issues/373) - build: fix build on appveyor CI \
* [Issue #374](https://github.com/json-c/json-c/issues/374) - Undefined symbols for architecture x86_64: \
* [Issue #375](https://github.com/json-c/json-c/issues/375) - what would happened when json_object_object_add add the same key \
* [Issue #376](https://github.com/json-c/json-c/issues/376) - Eclipse error \
* [Issue #377](https://github.com/json-c/json-c/issues/377) - on gcc 7.2.0 on my linux distribution with json-c  2013-04-02 source \
* [Issue #378](https://github.com/json-c/json-c/issues/378) - Eclipse: library (libjson-c) not found, but configured \
* [Issue #379](https://github.com/json-c/json-c/issues/379) - error: this statement may fall through \[-Werror=implicit-fallthrough=\] \
* [Issue #380](https://github.com/json-c/json-c/issues/380) - Build on Windows \
* [Issue #381](https://github.com/json-c/json-c/issues/381) - Fix makedist \
* [Issue #382](https://github.com/json-c/json-c/issues/382) - Memory leak for json_tokener_parse_ex for version 0.12.1 \
* [Issue #383](https://github.com/json-c/json-c/issues/383) - Fix a compiler warning. \
* [Issue #384](https://github.com/json-c/json-c/issues/384) - Fix a VS 2015 compiler warnings. \
