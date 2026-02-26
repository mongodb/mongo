This list was created with:

```
curl "https://api.github.com/search/issues?q=repo%3Ajson-c%2Fjson-c+closed%3A>2020-04-18+created%3A<2020-07-23&sort=created&order=asc&per_page=100&page=1" > issues1.out
jq -r '.items[] | "[" + .title + "](" + .url + ")" | tostring' issues?.out > issues.md
sed -e's,^\[ *\(.*\)\](https://api.github.com/.*/\([0-9].*\)),* [Issue #\2](https://github.com/json-c/json-c/issues/\2) - \1,' -i issues.md
#... manual editing ...

```

----

Issues and Pull Requests closed for the 0.15 release
(since commit 31ab57ca, the 0.14 branch point, 2020-04-19)

* [Issue #428](https://github.com/json-c/json-c/issues/428) - Added new_null() function
* [Issue #429](https://github.com/json-c/json-c/issues/429) - Conflict of interest between JSON_C_TO_STRING_SPACED and JSON_C_TO_STRING_PRETTY
* [Issue #451](https://github.com/json-c/json-c/issues/451) - Add option to disable HAVE___THREAD
* [Issue #471](https://github.com/json-c/json-c/issues/471) - create folders with mode 0755 when building
* [Issue #476](https://github.com/json-c/json-c/issues/476) - Add new function named json_object_new_string_noalloc
* [Issue #484](https://github.com/json-c/json-c/issues/484) - Add support for uint64
* [Issue #487](https://github.com/json-c/json-c/issues/487) - Any plans to make new release? (0.14)
* [Issue #493](https://github.com/json-c/json-c/issues/493) - Kdopen rename library
* [Issue #507](https://github.com/json-c/json-c/issues/507) - Double value -1.0 converts to integer in json_object_to_json_string()
* [Issue #508](https://github.com/json-c/json-c/issues/508) - Recommend enabling the `-fPIC` compiler flag by default
* [Issue #517](https://github.com/json-c/json-c/issues/517) - Lja mods
* [Issue #534](https://github.com/json-c/json-c/issues/534) - Both json-c and json-glib have json_object_get_type()
* [Issue #584](https://github.com/json-c/json-c/issues/584) - CMake: SOVERSION and the major library VERSION need to be in lockstep.
* [Issue #585](https://github.com/json-c/json-c/issues/585) - CMake: Do not install config.h, as it is not a public header file.
* [Issue #586](https://github.com/json-c/json-c/issues/586) - 10796 Segmentation fault
* [Issue #588](https://github.com/json-c/json-c/issues/588) - Broken RDRAND causes infinite looping
* [Issue #589](https://github.com/json-c/json-c/issues/589) - Detect broken RDRAND during initialization
* [Issue #590](https://github.com/json-c/json-c/issues/590) - Fix segmentation fault in CPUID check
* [Issue #591](https://github.com/json-c/json-c/issues/591) - Update README.md
* [Issue #592](https://github.com/json-c/json-c/issues/592) - Prevent out of boundary write on malicious input
* [Issue #593](https://github.com/json-c/json-c/issues/593) - Building both static and shared libraries
* [Issue #594](https://github.com/json-c/json-c/issues/594) - Some subsequent call of lh_get_hash not working
* [Issue #595](https://github.com/json-c/json-c/issues/595) - Support to build both static and shared libraries
* [Issue #596](https://github.com/json-c/json-c/issues/596) - QA Notice: Package triggers severe warnings
* [Issue #597](https://github.com/json-c/json-c/issues/597) - json_parse demo: fix and use usage() function
* [Issue #598](https://github.com/json-c/json-c/issues/598) - Turning off shared libs causes target duplication or build error
* [Issue #599](https://github.com/json-c/json-c/issues/599) - cannot add more than 11 objects. Is this a known issue?
* [Issue #600](https://github.com/json-c/json-c/issues/600) - Library name conflicts on Windows are back again
* [Issue #601](https://github.com/json-c/json-c/issues/601) - json_tokener_parse() in master sets errno=1 "Operation not permitted"
* [Issue #602](https://github.com/json-c/json-c/issues/602) - fix json_parse_uint64() internal error checking with errno
* [Issue #603](https://github.com/json-c/json-c/issues/603) - Backport of fixes from master branch.
* [Issue #604](https://github.com/json-c/json-c/issues/604) - commit f2e991a3419ee4078e8915e840b1a0d9003b349e breaks cross-compilation with mingw
* [Issue #605](https://github.com/json-c/json-c/issues/605) - Update to 0.15 release
* [Issue #606](https://github.com/json-c/json-c/issues/606) - Improved support for IBM operating systems
* [Issue #607](https://github.com/json-c/json-c/issues/607) - json-c-0.13.x: Fix CVE-2020-12762 - json-c through 0.14 has an integer overflow and out-of-bounds write ...
* [Issue #608](https://github.com/json-c/json-c/issues/608) - json-c-0.14: Fix CVE-2020-12762 - json-c through 0.14 has an integer overflow and out-of-bounds write ...
* [Issue #609](https://github.com/json-c/json-c/issues/609) - use unsigned types for sizes in lh_table and entries
* [Issue #610](https://github.com/json-c/json-c/issues/610) - let's not call lh_table_resize with INT_MAX
* [Issue #611](https://github.com/json-c/json-c/issues/611) - json-c-0.12.x: Fix CVE-2020-12762 - json-c through 0.14 has an integer overflow and out-of-bounds write ...
* [Issue #613](https://github.com/json-c/json-c/issues/613) - json-c-0.10: Fix CVE-2020-12762 - json-c through 0.14 has an integer overflow and out-of-bounds write ...
* [Issue #614](https://github.com/json-c/json-c/issues/614) - Prevent truncation on custom double formatters.
* [Issue #615](https://github.com/json-c/json-c/issues/615) - New release with security fix
* [Issue #616](https://github.com/json-c/json-c/issues/616) - Parsing fails if UTF-16 low surrogate pair is not in same chunk is the high pair
* [Issue #617](https://github.com/json-c/json-c/issues/617) - Add an option to disable the use of thread-local storage.
* [Issue #618](https://github.com/json-c/json-c/issues/618) - test_deep_copy: Fix assertion value.
* [Issue #619](https://github.com/json-c/json-c/issues/619) - CMake: Fix out-of-tree build for Doxygen documentation.
* [Issue #621](https://github.com/json-c/json-c/issues/621) - json-c and jansson libraries have symbol conflicts
* [Issue #622](https://github.com/json-c/json-c/issues/622) - doc: Move Doxyfile into doc subdir.
* [Issue #623](https://github.com/json-c/json-c/issues/623) - json_tokener_parse : Segmentation fault
* [Issue #626](https://github.com/json-c/json-c/issues/626) - Fixes for cmake 2.8.12 + link issue on AIX 6.1/cc 11.01
* [Issue #627](https://github.com/json-c/json-c/issues/627) - Compat fixes
* [Issue #628](https://github.com/json-c/json-c/issues/628) - get_cryptgenrandom_seed: compat with old windows + fallback
* [Issue #629](https://github.com/json-c/json-c/issues/629) - [0.12] Remove the Visual Studio project file
* [Issue #630](https://github.com/json-c/json-c/issues/630) - Linking with Windows MINGW not working
* [Issue #632](https://github.com/json-c/json-c/issues/632) - Json object split
* [Issue #633](https://github.com/json-c/json-c/issues/633) - fix issue 616: support the surrogate pair in split file.
* [Issue #634](https://github.com/json-c/json-c/issues/634) - Issue #508: `-fPIC` to link libjson-c.a with libs
* [Issue #635](https://github.com/json-c/json-c/issues/635) - expression has no effect warning in json_tokener.c
* [Issue #636](https://github.com/json-c/json-c/issues/636) - json_object_get_string free str memory
* [Issue #637](https://github.com/json-c/json-c/issues/637) - json_object_put()  has 'double free or corruption (out) '
* [Issue #638](https://github.com/json-c/json-c/issues/638) - json-c/json_object.c:50:2: error: #error Unable to determine size of ssize_t
* [Issue #639](https://github.com/json-c/json-c/issues/639) - build: Add a symbol version to all exported symbols
* [Issue #640](https://github.com/json-c/json-c/issues/640) - Fix build issues with SSIZE_MAX on 64bit Linux
* [Issue #641](https://github.com/json-c/json-c/issues/641) - Formal verification of your test suite
* [Issue #642](https://github.com/json-c/json-c/issues/642) - Please provide more precise informations about when to call json_object_put
* [Issue #643](https://github.com/json-c/json-c/issues/643) - not able to compare with string
* [Issue #644](https://github.com/json-c/json-c/issues/644) - Why src->_userdata not checked before calling strdup?
* [Issue #645](https://github.com/json-c/json-c/issues/645) - Misuse of tolower() in json_tokener.c
* [Issue #646](https://github.com/json-c/json-c/issues/646) - Cast to unsigned char instead of int when calling tolower (Fixes #645)

