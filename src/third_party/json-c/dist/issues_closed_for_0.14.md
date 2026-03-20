This list was created with:

```
curl https://api.github.com/search/issues?q="repo%3Ajson-c%2Fjson-c+closed%3A>2017-12-07+created%3A<2020-04-17&sort=created&order=asc&per_page=400&page=1" > issues1.out
curl https://api.github.com/search/issues?q="repo%3Ajson-c%2Fjson-c+closed%3A>2017-12-07+created%3A<2020-04-17&sort=created&order=asc&per_page=400&page=2" > issues2.out
curl https://api.github.com/search/issues?q="repo%3Ajson-c%2Fjson-c+closed%3A>2017-12-07+created%3A<2020-04-17&sort=created&order=asc&per_page=400&page=3" > issues3.out
jq -r '.items[] | "[" + .title + "](" + .url + ")" | tostring' issues?.out > issues.md
sed -e's,^\[ *\(.*\)\](https://api.github.com/.*/\([0-9].*\)),[Issue #\2](https://github.com/json-c/json-c/issues/\2) - \1,' -i issues.md
#... manual editing ...
```

----

Issues and Pull Requests closed for the 0.14 release (since commit d582d3a(2017-12-07) to a911439(2020-04-17))


* [Issue #122](https://github.com/json-c/json-c/issues/122) - Add utf-8 validation when parsing strings. \
* [Issue #139](https://github.com/json-c/json-c/issues/139) - json_object_from_file cannot accept max_depth \
* [Issue #143](https://github.com/json-c/json-c/issues/143) - RFE / enhancement for full 64-bit signed/unsigned support \
* [Issue #147](https://github.com/json-c/json-c/issues/147) - Please introduce soname bump if API changed \
* [Issue #166](https://github.com/json-c/json-c/issues/166) - Need a way to specify nesting depth when opening JSON file \
* [Issue #226](https://github.com/json-c/json-c/issues/226) - There is no json_object_new_null() \
* [Issue #314](https://github.com/json-c/json-c/issues/314) - new release ? \
* [Issue #326](https://github.com/json-c/json-c/issues/326) - Please extend api json_object_get_uint64 \
* [Issue #334](https://github.com/json-c/json-c/issues/334) - Switch json-c builds to use CMake \
* [Issue #386](https://github.com/json-c/json-c/issues/386) - Makefile: Add ACLOCAL_AMFLAGS \
* [Issue #387](https://github.com/json-c/json-c/issues/387) - doc: Use other doxygen feature to specify mainpage \
* [Issue #388](https://github.com/json-c/json-c/issues/388) - json_object: Add size_t json_object_sizeof() \
* [Issue #389](https://github.com/json-c/json-c/issues/389) - json_object: Avoid double free (and thus a segfault) when ref_count gets < 0 \
* [Issue #390](https://github.com/json-c/json-c/issues/390) - json_object: Add const size_t json_c_object_sizeof() \
* [Issue #391](https://github.com/json-c/json-c/issues/391) - Fix non-GNUC define for JSON_C_CONST_FUNCTION \
* [Issue #392](https://github.com/json-c/json-c/issues/392) - json_object: Avoid invalid free (and thus a segfault) when ref_count gets < 0 \
* [Issue #393](https://github.com/json-c/json-c/issues/393) - json_object_private: Use unsigned 32-bit integer type for refcount \
* [Issue #394](https://github.com/json-c/json-c/issues/394) - Problem serializing double \
* [Issue #395](https://github.com/json-c/json-c/issues/395) - Key gets modified if it contains "\" \
* [Issue #396](https://github.com/json-c/json-c/issues/396) - Build failure with no threads uClibc toolchain \
* [Issue #397](https://github.com/json-c/json-c/issues/397) - update json object with key. \
* [Issue #398](https://github.com/json-c/json-c/issues/398) - Build failed. \
* [Issue #399](https://github.com/json-c/json-c/issues/399) - Avoid uninitialized variable warnings \
* [Issue #400](https://github.com/json-c/json-c/issues/400) - How to generate static lib (.a) \
* [Issue #401](https://github.com/json-c/json-c/issues/401) - Warnings with Valgrind \
* [Issue #402](https://github.com/json-c/json-c/issues/402) - Add fuzzers from OSS-Fuzz \
* [Issue #403](https://github.com/json-c/json-c/issues/403) - Segmentation fault when double quotes is used \
* [Issue #404](https://github.com/json-c/json-c/issues/404) - valgrind: memory leak \
* [Issue #405](https://github.com/json-c/json-c/issues/405) - Missing API to determine an object is empty \
* [Issue #406](https://github.com/json-c/json-c/issues/406) - Undefine NDEBUG for tests \
* [Issue #407](https://github.com/json-c/json-c/issues/407) - json_tokener_parse is crash \
* [Issue #408](https://github.com/json-c/json-c/issues/408) - bug in array_list_del_idx when array_list_length()==1 \
* [Issue #410](https://github.com/json-c/json-c/issues/410) - Fixed typos \
* [Issue #411](https://github.com/json-c/json-c/issues/411) - Crash- signal SIGSEGV, Segmentation fault. ../sysdeps/x86_64/strlen.S: No such file or directory. \
* [Issue #412](https://github.com/json-c/json-c/issues/412) - json_type changes during inter process communication. \
* [Issue #413](https://github.com/json-c/json-c/issues/413) - how to read object of type `json_object *` in c++ \
* [Issue #414](https://github.com/json-c/json-c/issues/414) - [Question] How JSON-c stores the serialized data in memory? \
* [Issue #415](https://github.com/json-c/json-c/issues/415) - Resolve windows name conflict \
* [Issue #416](https://github.com/json-c/json-c/issues/416) - segmentation fault  in json_tokener_parse \
* [Issue #417](https://github.com/json-c/json-c/issues/417) - json_tokener_parse  json_object_object_get_ex with string value which is json string \
* [Issue #418](https://github.com/json-c/json-c/issues/418) - json_object_from_* return value documented incorrectly \
* [Issue #419](https://github.com/json-c/json-c/issues/419) - Suggestion: document (and define) that json_object_put() accepts NULL pointer to object \
* [Issue #420](https://github.com/json-c/json-c/issues/420) - arraylist: Fixed names of parameters for callback function \
* [Issue #421](https://github.com/json-c/json-c/issues/421) - install json_object_iterator.h header file \
* [Issue #422](https://github.com/json-c/json-c/issues/422) - json_object_get_double() does not set errno when there is no valid conversion \
* [Issue #423](https://github.com/json-c/json-c/issues/423) - memory leak \
* [Issue #424](https://github.com/json-c/json-c/issues/424) - Parse string contains "\" or "/" errors \
* [Issue #425](https://github.com/json-c/json-c/issues/425) - what this is? \
* [Issue #426](https://github.com/json-c/json-c/issues/426) - __deprecated not supported on clang. \
* [Issue #427](https://github.com/json-c/json-c/issues/427) - CMake: builds involving this target will not be correct \
* [Issue #430](https://github.com/json-c/json-c/issues/430) - json_object_object_del() and Segmentation fault \
* [Issue #431](https://github.com/json-c/json-c/issues/431) - cmake: Bump required version \
* [Issue #432](https://github.com/json-c/json-c/issues/432) - The real CMake support. \
* [Issue #433](https://github.com/json-c/json-c/issues/433) - The real CMake support. \
* [Issue #434](https://github.com/json-c/json-c/issues/434) - The real CMake support \
* [Issue #435](https://github.com/json-c/json-c/issues/435) - json_object_object_del() segmentation fault \
* [Issue #436](https://github.com/json-c/json-c/issues/436) - Improve pkgconfig setting \
* [Issue #437](https://github.com/json-c/json-c/issues/437) - Bad link in README.md \
* [Issue #438](https://github.com/json-c/json-c/issues/438) - Bad link in README.html \
* [Issue #439](https://github.com/json-c/json-c/issues/439) - reserved identifier violation \
* [Issue #440](https://github.com/json-c/json-c/issues/440) - Use of angle brackets around file names for include statements \
* [Issue #441](https://github.com/json-c/json-c/issues/441) - fix c flag loss during cmake building \
* [Issue #442](https://github.com/json-c/json-c/issues/442) - error  in configure file \
* [Issue #443](https://github.com/json-c/json-c/issues/443) - remove pretty spaces when using pretty tabs \
* [Issue #444](https://github.com/json-c/json-c/issues/444) - Document refcount of json_tokener_parse_ex return \
* [Issue #445](https://github.com/json-c/json-c/issues/445) - Add missing "make check" target to cmake config \
* [Issue #446](https://github.com/json-c/json-c/issues/446) - Forward slashes get escaped \
* [Issue #448](https://github.com/json-c/json-c/issues/448) - Buffer overflow in json-c \
* [Issue #449](https://github.com/json-c/json-c/issues/449) - Need of json_type_int64 returned by json_object_get_type() \
* [Issue #450](https://github.com/json-c/json-c/issues/450) - Allow use json-c cmake as subproject \
* [Issue #452](https://github.com/json-c/json-c/issues/452) - Update README.md \
* [Issue #453](https://github.com/json-c/json-c/issues/453) - Fixed misalignment in JSON string due to space after \n being printed... \
* [Issue #454](https://github.com/json-c/json-c/issues/454) - json_object_private: save 8 bytes in struct json_object in 64-bit arc… \
* [Issue #455](https://github.com/json-c/json-c/issues/455) - index.html:fix dead link \
* [Issue #456](https://github.com/json-c/json-c/issues/456) - STYLE.txt:remove executable permissions \
* [Issue #457](https://github.com/json-c/json-c/issues/457) - .gitignore:add build directory \
* [Issue #458](https://github.com/json-c/json-c/issues/458) - README.md:fix dead "file.html" link \
* [Issue #459](https://github.com/json-c/json-c/issues/459) - README.html:fix link to Doxygen docs, remove WIN32 link \
* [Issue #460](https://github.com/json-c/json-c/issues/460) - No docs for json_object_new_string_len() \
* [Issue #461](https://github.com/json-c/json-c/issues/461) - json_object.c:set errno in json_object_get_double() \
* [Issue #462](https://github.com/json-c/json-c/issues/462) - json_object.h:document json_object_new_string_len() \
* [Issue #463](https://github.com/json-c/json-c/issues/463) - please check newlocale api first argument valuse. \
* [Issue #465](https://github.com/json-c/json-c/issues/465) - CMakeLists.txt doesn't contain json_object_iterator.h which json.h includes \
* [Issue #466](https://github.com/json-c/json-c/issues/466) - configure:3610: error: C compiler cannot create executables \
* [Issue #467](https://github.com/json-c/json-c/issues/467) - Fix compiler warnings \
* [Issue #468](https://github.com/json-c/json-c/issues/468) - Fix compiler warnings \
* [Issue #469](https://github.com/json-c/json-c/issues/469) - Build under alpine with pecl install & docker-php-ext-enable? \
* [Issue #470](https://github.com/json-c/json-c/issues/470) - cfuhash_foreach_remove doesn't upate cfuhash_num_entries \
* [Issue #472](https://github.com/json-c/json-c/issues/472) - Segmentation fault in json_object_iter_begin \
* [Issue #473](https://github.com/json-c/json-c/issues/473) - Convert ChangeLog to valid UTF-8 encoding. \
* [Issue #474](https://github.com/json-c/json-c/issues/474) - Installation directories empty with CMake in pkg-config. \
* [Issue #475](https://github.com/json-c/json-c/issues/475) - improvement proposal for json_object_object_foreach \
* [Issue #477](https://github.com/json-c/json-c/issues/477) - Hang/Crash with large strings \
* [Issue #478](https://github.com/json-c/json-c/issues/478) - json_object_get_string_len returns 0 when value is number \
* [Issue #479](https://github.com/json-c/json-c/issues/479) - I want to use it in iOS or Android but I can't compile \
* [Issue #480](https://github.com/json-c/json-c/issues/480) - json-c-0.12.1  failed making from source code \
* [Issue #481](https://github.com/json-c/json-c/issues/481) - error while loading shared libraries: libjson-c.so.4 \
* [Issue #482](https://github.com/json-c/json-c/issues/482) - Error "double free or corruption" after free() \
* [Issue #483](https://github.com/json-c/json-c/issues/483) - compatible with rarely-used Chinese characters in GBK charset \
* [Issue #485](https://github.com/json-c/json-c/issues/485) - Install CMake module files \
* [Issue #486](https://github.com/json-c/json-c/issues/486) - In the case of negative double value, it is formatted without including ".0" \
* [Issue #488](https://github.com/json-c/json-c/issues/488) - Some APIs are not exported when built as shared lib on Win32 \
* [Issue #489](https://github.com/json-c/json-c/issues/489) - Don't use -Werror by default \
* [Issue #490](https://github.com/json-c/json-c/issues/490) - do not compile with -Werror by default \
* [Issue #491](https://github.com/json-c/json-c/issues/491) - build: add option --disable-werror to configure \
* [Issue #492](https://github.com/json-c/json-c/issues/492) - lack some quick usage in readme \
* [Issue #494](https://github.com/json-c/json-c/issues/494) - Code generator? \
* [Issue #495](https://github.com/json-c/json-c/issues/495) - README.md:fix 2 typos \
* [Issue #496](https://github.com/json-c/json-c/issues/496) - json_pointer.h:suggest minor grammar improvement for pointer doc \
* [Issue #497](https://github.com/json-c/json-c/issues/497) - add common header for all tests \
* [Issue #498](https://github.com/json-c/json-c/issues/498) - double_serializer_test fails (with valgrind) \
* [Issue #499](https://github.com/json-c/json-c/issues/499) - .travis.yml:test on more recent clang and gcc versions \
* [Issue #500](https://github.com/json-c/json-c/issues/500) - test/Makefile.am:add missing deps for test1 and test2 \
* [Issue #501](https://github.com/json-c/json-c/issues/501) - undefine NDEBUG for tests \
* [Issue #502](https://github.com/json-c/json-c/issues/502) - configure error \
* [Issue #503](https://github.com/json-c/json-c/issues/503) - json-c retuns OK when Invalid json string is passed \
* [Issue #504](https://github.com/json-c/json-c/issues/504) - json_object_put coredump \
* [Issue #505](https://github.com/json-c/json-c/issues/505) - Add vcpkg installation instructions \
* [Issue #506](https://github.com/json-c/json-c/issues/506) - Cannot parse more than one object \
* [Issue #509](https://github.com/json-c/json-c/issues/509) - Sometimes a double value is not serialized \
* [Issue #510](https://github.com/json-c/json-c/issues/510) - Bump so-name and improve CMake \
* [Issue #511](https://github.com/json-c/json-c/issues/511) - Reduce lines for better optimization \
* [Issue #512](https://github.com/json-c/json-c/issues/512) - Properly append to CMAKE_C_FLAGS string \
* [Issue #513](https://github.com/json-c/json-c/issues/513) - What does `userdata` means?And what is the case we can use it? \
* [Issue #514](https://github.com/json-c/json-c/issues/514) - Json c 0.13 \
* [Issue #515](https://github.com/json-c/json-c/issues/515) - Mies suomesta fixes segfaults and logic errors \
* [Issue #516](https://github.com/json-c/json-c/issues/516) - Lja slight mods \
* [Issue #518](https://github.com/json-c/json-c/issues/518) - Escape character  "\\003\", get unexpected value \
* [Issue #519](https://github.com/json-c/json-c/issues/519) - Add test case obj token \
* [Issue #520](https://github.com/json-c/json-c/issues/520) - Adding type uint64 \
* [Issue #521](https://github.com/json-c/json-c/issues/521) - build cmake windows 10 \
* [Issue #522](https://github.com/json-c/json-c/issues/522) - update json_visit testcase \
* [Issue #523](https://github.com/json-c/json-c/issues/523) - update tsetcase for tokener_c \
* [Issue #524](https://github.com/json-c/json-c/issues/524) - Increase coverage \
* [Issue #525](https://github.com/json-c/json-c/issues/525) - update pointer test case \
* [Issue #526](https://github.com/json-c/json-c/issues/526) - Increased the test coverage of printbuf.c 82% to 92%. \
* [Issue #527](https://github.com/json-c/json-c/issues/527) - Arraylist testcase \
* [Issue #528](https://github.com/json-c/json-c/issues/528) - Solve issue #108. Skip \u0000 while parsing. \
* [Issue #529](https://github.com/json-c/json-c/issues/529) - Increased the test coverage of json_c_version.c 0% to 100%. \
* [Issue #530](https://github.com/json-c/json-c/issues/530) - validate utf-8 string before parse \
* [Issue #531](https://github.com/json-c/json-c/issues/531) - validate utf-8 string \
* [Issue #532](https://github.com/json-c/json-c/issues/532) - json_object_object_get_ex returning the original object \
* [Issue #533](https://github.com/json-c/json-c/issues/533) - Fix "make check" \
* [Issue #535](https://github.com/json-c/json-c/issues/535) - short string optimization: excessive array length \
* [Issue #536](https://github.com/json-c/json-c/issues/536) - add json_object_new_null() \
* [Issue #538](https://github.com/json-c/json-c/issues/538) - update shortstring and arraylist parameters \
* [Issue #539](https://github.com/json-c/json-c/issues/539) - double serializes to the old value after set_double \
* [Issue #541](https://github.com/json-c/json-c/issues/541) - add coveralls auto tool to json-c \
* [Issue #542](https://github.com/json-c/json-c/issues/542) - add uint64 data to json-c \
* [Issue #543](https://github.com/json-c/json-c/issues/543) - Readme \
* [Issue #544](https://github.com/json-c/json-c/issues/544) - Increase distcheck target in cmake \
* [Issue #545](https://github.com/json-c/json-c/issues/545) - add doc target in cmake \
* [Issue #546](https://github.com/json-c/json-c/issues/546) - Add uninstall target in cmake \
* [Issue #547](https://github.com/json-c/json-c/issues/547) - modify json-c default build type, and fix up the assert() errors in t… \
* [Issue #548](https://github.com/json-c/json-c/issues/548) - Solve some problems about cmake build type (debug/release) \
* [Issue #549](https://github.com/json-c/json-c/issues/549) - lib installation issues \
* [Issue #550](https://github.com/json-c/json-c/issues/550) - Format codes with clang-format tool? \
* [Issue #551](https://github.com/json-c/json-c/issues/551) - Allow hexadecimal number format convention parsing \
* [Issue #553](https://github.com/json-c/json-c/issues/553) - Fix/clang ubsan \
* [Issue #554](https://github.com/json-c/json-c/issues/554) - RFC 8259 compatibility mode \
* [Issue #555](https://github.com/json-c/json-c/issues/555) - Format json-c with clang-format tool \
* [Issue #556](https://github.com/json-c/json-c/issues/556) - Fixes various Wreturn-type and Wimplicit-fallthrough errors on Mingw-w64 \
* [Issue #557](https://github.com/json-c/json-c/issues/557) - Add option in CMAKE to not build documentation \
* [Issue #558](https://github.com/json-c/json-c/issues/558) - modify the doc target message \
* [Issue #559](https://github.com/json-c/json-c/issues/559) - json_c_visit() not exported on Windows \
* [Issue #560](https://github.com/json-c/json-c/issues/560) - error: implicit declaration of function '_strtoi64' \
* [Issue #561](https://github.com/json-c/json-c/issues/561) - add the badge in README.md and test the coveralls \
* [Issue #562](https://github.com/json-c/json-c/issues/562) - Bugfix and testcases supplements \
* [Issue #563](https://github.com/json-c/json-c/issues/563) - Changed order of calloc args to match stdlib \
* [Issue #564](https://github.com/json-c/json-c/issues/564) - Remove autogenerated files \
* [Issue #565](https://github.com/json-c/json-c/issues/565) - test the CI and ignore this PR \
* [Issue #566](https://github.com/json-c/json-c/issues/566) - add the json_types.h to Makefile.am \
* [Issue #567](https://github.com/json-c/json-c/issues/567) - Install json_types.h with autotools build as well. \
* [Issue #568](https://github.com/json-c/json-c/issues/568) - Adding better support to MinGW \
* [Issue #569](https://github.com/json-c/json-c/issues/569) - Handling of -Bsymbolic-function in CMakeLists.txt is deficient \
* [Issue #571](https://github.com/json-c/json-c/issues/571) - CMake: Bump SONAME to 5. \
* [Issue #572](https://github.com/json-c/json-c/issues/572) - Small fixes to CMakeLists \
* [Issue #573](https://github.com/json-c/json-c/issues/573) - Fix coveralls submission. \
* [Issue #574](https://github.com/json-c/json-c/issues/574) - autogen.sh missing from repository \
* [Issue #575](https://github.com/json-c/json-c/issues/575) - Small cosmetics. \
* [Issue #576](https://github.com/json-c/json-c/issues/576) - Test coverage for json_c_version. \
* [Issue #577](https://github.com/json-c/json-c/issues/577) - Be verbose on failing json_c_version test. \
* [Issue #578](https://github.com/json-c/json-c/issues/578) - CMake: Install pkgconfig file in proper location by default \
* [Issue #579](https://github.com/json-c/json-c/issues/579) - Enforce strict prototypes. \
* [Issue #580](https://github.com/json-c/json-c/issues/580) - Fix CMake tests for enforced strict prototypes. \
* [Issue #581](https://github.com/json-c/json-c/issues/581) - CMakeLists: do not enforce strict prototypes on Windows. \
