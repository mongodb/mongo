This list was created with:

```
PREV=2020-07-23
NOW=2022-04-13
curl "https://api.github.com/search/issues?q=repo%3Ajson-c%2Fjson-c+closed%3A>${PREV}+created%3A<${NOW}&sort=created&order=asc&per_page=100&page=1" > issues1.out
jq -r '.items[] | "[" + .title + "](" + .url + ")" | tostring' issues?.out > issues.md
sed -e's,^\[ *\(.*\)\](https://api.github.com/.*/\([0-9].*\)),* [Issue #\2](https://github.com/json-c/json-c/issues/\2) - \1,' -i issues.md
cat issues.md >> issues_closed_for_0.16.md
```

* [Issue #464](https://github.com/json-c/json-c/issues/464) - Speed up parsing and object creation
* [Issue #540](https://github.com/json-c/json-c/issues/540) - request: json_init_library
* [Issue #631](https://github.com/json-c/json-c/issues/631) - New 0.14 release requests
* [Issue #647](https://github.com/json-c/json-c/issues/647) - "cmake -DCMAKE_BUILD_TYPE=Release" fails with error: 'cint64' may be used uninitialized
* [Issue #648](https://github.com/json-c/json-c/issues/648) - Fix "may be used uninitialized" Release build failure
* [Issue #649](https://github.com/json-c/json-c/issues/649) - json-c tag 0.15 tarball contains a file doc/Doxyfile and generated doxygen files in doc/html
* [Issue #650](https://github.com/json-c/json-c/issues/650) - README: fix spelling errors
* [Issue #651](https://github.com/json-c/json-c/issues/651) - Getrandom
* [Issue #652](https://github.com/json-c/json-c/issues/652) - Waste memory
* [Issue #653](https://github.com/json-c/json-c/issues/653) - Make the documentation build reproducibly
* [Issue #654](https://github.com/json-c/json-c/issues/654) - A stack-buffer-overflow in json_parse.c:89:44
* [Issue #655](https://github.com/json-c/json-c/issues/655) - json_parse: Fix read past end of buffer
* [Issue #656](https://github.com/json-c/json-c/issues/656) - Fixed warnings
* [Issue #657](https://github.com/json-c/json-c/issues/657) - Use GRND_NONBLOCK with getrandom.
* [Issue #658](https://github.com/json-c/json-c/issues/658) - json_object_get_boolean() returns wrong result for objects and arrays
* [Issue #659](https://github.com/json-c/json-c/issues/659) - fix json_object_get_boolean() to behave like documented
* [Issue #660](https://github.com/json-c/json-c/issues/660) - Validate size arguments in arraylist functions.
* [Issue #661](https://github.com/json-c/json-c/issues/661) - Cleanup of some code parts
* [Issue #662](https://github.com/json-c/json-c/issues/662) - Prevent signed overflow in get_time_seed
* [Issue #663](https://github.com/json-c/json-c/issues/663) - Properly format errnos in _json_c_strerror
* [Issue #664](https://github.com/json-c/json-c/issues/664) - Limit strings at INT_MAX length
* [Issue #665](https://github.com/json-c/json-c/issues/665) - Handle more allocation failures in json_tokener* functions
* [Issue #666](https://github.com/json-c/json-c/issues/666) - test1 json_object_new_array_ext test is failing
* [Issue #667](https://github.com/json-c/json-c/issues/667) - Fixed test1 regression.
* [Issue #670](https://github.com/json-c/json-c/issues/670) - Created Stone-Paper-Scissor Game by C language
* [Issue #672](https://github.com/json-c/json-c/issues/672) - Calling exit() after failure to generate random seed
* [Issue #673](https://github.com/json-c/json-c/issues/673) - switchcasemenuproject
* [Issue #674](https://github.com/json-c/json-c/issues/674) - random_seed: on error, continue to next method
* [Issue #682](https://github.com/json-c/json-c/issues/682) - libjson-c-dev vs libjson-c3
* [Issue #683](https://github.com/json-c/json-c/issues/683) - [Question] Is it possible to clear a ptr of json_object?
* [Issue #684](https://github.com/json-c/json-c/issues/684) - json_tokener_parse_verbose failed with core dump
* [Issue #685](https://github.com/json-c/json-c/issues/685) - json_tokener_parse memory leak?
* [Issue #689](https://github.com/json-c/json-c/issues/689) - fix compilation with clang
* [Issue #690](https://github.com/json-c/json-c/issues/690) - "1," produces an object with int 1; "1" produces a null object
* [Issue #691](https://github.com/json-c/json-c/issues/691) - failed tests
* [Issue #692](https://github.com/json-c/json-c/issues/692) - patch to add arc4random
* [Issue #693](https://github.com/json-c/json-c/issues/693) - Optional parameter for packing as array
* [Issue #694](https://github.com/json-c/json-c/issues/694) - fix invalid unsigned arithmetic.
* [Issue #695](https://github.com/json-c/json-c/issues/695) - /tmp/json-c/random_seed.c:327:6: error
* [Issue #696](https://github.com/json-c/json-c/issues/696) - To avoid target exe file export JSON functions.
* [Issue #697](https://github.com/json-c/json-c/issues/697) - json_object_get_string() return value truncated when assigning it to a pointer type in Win32 App
* [Issue #698](https://github.com/json-c/json-c/issues/698) - Feature request: set allocator
* [Issue #699](https://github.com/json-c/json-c/issues/699) - Linking to libjson-c Issue
* [Issue #700](https://github.com/json-c/json-c/issues/700) - Fix unused variable for Win32 build in random_seed.c
* [Issue #701](https://github.com/json-c/json-c/issues/701) - [RFC] json_pointer: allow the feature to be disabled
* [Issue #703](https://github.com/json-c/json-c/issues/703) - Fix vasprintf fallback
* [Issue #706](https://github.com/json-c/json-c/issues/706) - Check __STDC_VERSION__ is defined before checking its value
* [Issue #707](https://github.com/json-c/json-c/issues/707) - How to build json-c-0.15 for arm arch
* [Issue #708](https://github.com/json-c/json-c/issues/708) - direct access to elements
* [Issue #709](https://github.com/json-c/json-c/issues/709) - Include guards not namespaced / build errors for debug.h with openNDS
* [Issue #710](https://github.com/json-c/json-c/issues/710) - 'file system sandbox blocked mmap()' error on iOS
* [Issue #711](https://github.com/json-c/json-c/issues/711) - creating a json object 
* [Issue #712](https://github.com/json-c/json-c/issues/712) - building json-c using cmake for ESP32
* [Issue #713](https://github.com/json-c/json-c/issues/713) - When value converted to char* can not compare it with another value
* [Issue #714](https://github.com/json-c/json-c/issues/714) - Add AfterCaseLabel to .clang-format
* [Issue #716](https://github.com/json-c/json-c/issues/716) - Fixed cmake command
* [Issue #717](https://github.com/json-c/json-c/issues/717) - Cmake is able delete all files by "clean" target
* [Issue #718](https://github.com/json-c/json-c/issues/718) - CMake create uninstall target if unix generator is used
* [Issue #719](https://github.com/json-c/json-c/issues/719) - Parsing multiple JSON strings
* [Issue #722](https://github.com/json-c/json-c/issues/722) - Fix use-after-free in json_tokener_new_ex()
* [Issue #723](https://github.com/json-c/json-c/issues/723) - if set __stdcall (/Gz)
* [Issue #724](https://github.com/json-c/json-c/issues/724) - #723
* [Issue #725](https://github.com/json-c/json-c/issues/725) - json_object_from_file()  execution segment error
* [Issue #726](https://github.com/json-c/json-c/issues/726) - fix cmake version for tests
* [Issue #727](https://github.com/json-c/json-c/issues/727) - Really use prefix JSON_C_OBJECT_ADD_
* [Issue #728](https://github.com/json-c/json-c/issues/728) - DRAFT PROPOSAL - Add option JSON_C_OBJECT_ADD_IF_NOT_NULL
* [Issue #729](https://github.com/json-c/json-c/issues/729) - * don't assume includedir
* [Issue #731](https://github.com/json-c/json-c/issues/731) - Json-c Error
* [Issue #732](https://github.com/json-c/json-c/issues/732) - Fix/static include dirs
* [Issue #734](https://github.com/json-c/json-c/issues/734) - Newer appveyor config for VS2022 etc...
* [Issue #735](https://github.com/json-c/json-c/issues/735) - Add policy_max to minimum required cmake version
* [Issue #736](https://github.com/json-c/json-c/issues/736) - json_object.c:308: json_object_put: Assertion `jso->_ref_count > 0' failed
* [Issue #737](https://github.com/json-c/json-c/issues/737) - Fix typo in README
* [Issue #738](https://github.com/json-c/json-c/issues/738) - General question - Is there an SLA for handling newly detected security issues?
* [Issue #739](https://github.com/json-c/json-c/issues/739) - json_escape_str(): avoid harmless unsigned integer overflow
* [Issue #741](https://github.com/json-c/json-c/issues/741) - json_type_to_name(): use correct printf() formatter
* [Issue #742](https://github.com/json-c/json-c/issues/742) - json_object_copy_serializer_data(): add assertion
* [Issue #743](https://github.com/json-c/json-c/issues/743) - Cmd adb root
* [Issue #744](https://github.com/json-c/json-c/issues/744) - Close file on error path.
* [Issue #745](https://github.com/json-c/json-c/issues/745) - vasprintf(): avoid out of memory accesses
* [Issue #746](https://github.com/json-c/json-c/issues/746) - Fix typos in code comments and ChangeLog
* [Issue #747](https://github.com/json-c/json-c/issues/747) - json_object_put: Assertion `jso->_ref_count > 0' failed
* [Issue #748](https://github.com/json-c/json-c/issues/748) - sprintbuf(): test for all vsnprintf error values
* [Issue #749](https://github.com/json-c/json-c/issues/749) - sprintbuf(): handle printbuf_memappend errors
* [Issue #750](https://github.com/json-c/json-c/issues/750) - printbuf_memset(): set gaps to zero
* [Issue #751](https://github.com/json-c/json-c/issues/751) - printbuf: do not allow invalid arguments
* [Issue #752](https://github.com/json-c/json-c/issues/752) - Fix typos
* [Issue #753](https://github.com/json-c/json-c/issues/753) - CTest failed in MSVC build
* [Issue #754](https://github.com/json-c/json-c/issues/754) - Minor improvements to documentation
* [Issue #755](https://github.com/json-c/json-c/issues/755) - Fix error messages
* [Issue #758](https://github.com/json-c/json-c/issues/758) - Preserve context if out of memory
* [Issue #760](https://github.com/json-c/json-c/issues/760) - Code style: removed unneeded double-quotes
* [Issue #761](https://github.com/json-c/json-c/issues/761) - Last commit merged to master breaks compilation
* [Issue #762](https://github.com/json-c/json-c/issues/762) - how to merge two jsons by json-c
* [Issue #763](https://github.com/json-c/json-c/issues/763) - Question: sort_fn arguments
* [Issue #764](https://github.com/json-c/json-c/issues/764) - Make test fail on test case test_util_file
