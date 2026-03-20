This list was created with:

```
PREV=2022-04-13
NOW=2023-08-12
curl "https://api.github.com/search/issues?q=repo%3Ajson-c%2Fjson-c+closed%3A>${PREV}+created%3A<${NOW}&sort=created&order=asc&per_page=100&page=1" > issues1.out
jq -r '.items[] | "[" + .title + "](" + .url + ")" | tostring' issues?.out > issues.md
sed -e's,^\[ *\(.*\)\](https://api.github.com/.*/\([0-9].*\)),* [Issue #\2](https://github.com/json-c/json-c/issues/\2) - \1,' -i issues.md
cat issues.md >> issues_closed_for_0.17.md
```

* [Issue #191](https://github.com/json-c/json-c/issues/191) - Override int64 to only display uint64 strings
* [Issue #537](https://github.com/json-c/json-c/issues/537) - Replace '\0' only when parsing key, not change data in value.
* [Issue #570](https://github.com/json-c/json-c/issues/570) - Figure out what needs to be done with Android.configure.mk
* [Issue #587](https://github.com/json-c/json-c/issues/587) - Store the hashValue to avoid repeating the hash calculation during the hash resize.
* [Issue #612](https://github.com/json-c/json-c/issues/612) - json-c-0.11: Fix CVE-2020-12762 - json-c through 0.14 has an integer overflow and out-of-bounds write ...
* [Issue #620](https://github.com/json-c/json-c/issues/620) - Introduce json_object_new_string_{ext,noalloc}().
* [Issue #624](https://github.com/json-c/json-c/issues/624) - json-c-0.14: Detect broken RDRAND during initialization.
* [Issue #625](https://github.com/json-c/json-c/issues/625) - json-c-0.13.x: Detect broken RDRAND during initialization.
* [Issue #668](https://github.com/json-c/json-c/issues/668) - Memory usage regression due to newlocal() on older FreeBSD releases
* [Issue #676](https://github.com/json-c/json-c/issues/676) - dereferencing type-punned pointer might break strict-aliasing rules [-Werror=strict-aliasing]
* [Issue #677](https://github.com/json-c/json-c/issues/677) - Naming conflict when using both json-c and jansson
* [Issue #679](https://github.com/json-c/json-c/issues/679) - Let json-c be used with obsolete compilers
* [Issue #681](https://github.com/json-c/json-c/issues/681) - json_tokener_parse_ex: `null` (4 bytes) only parses as valid JSON when passed with null terminator (5 bytes). Documentation issue?
* [Issue #686](https://github.com/json-c/json-c/issues/686) - Remove dependency on libM::getrandom
* [Issue #687](https://github.com/json-c/json-c/issues/687) - Does not build on Apple Silicon M1
* [Issue #688](https://github.com/json-c/json-c/issues/688) - json-c-0.15-nodoc.tar.gz build fails
* [Issue #702](https://github.com/json-c/json-c/issues/702) - json_patch: add first implementation only with patch application
* [Issue #704](https://github.com/json-c/json-c/issues/704) - add json_object_array_insert_idx() + test-cases + fix json_pointer doc-strings
* [Issue #705](https://github.com/json-c/json-c/issues/705) - segmentation fault on json-c parsing methods in cross compiled target
* [Issue #721](https://github.com/json-c/json-c/issues/721) - cmake test fails with building json-c with icc
* [Issue #730](https://github.com/json-c/json-c/issues/730) - Need a comparison with other JSON libraries in C
* [Issue #733](https://github.com/json-c/json-c/issues/733) - Official release? 1.0?
* [Issue #756](https://github.com/json-c/json-c/issues/756) - Question: Is there any way to build this with Gnu Make?
* [Issue #757](https://github.com/json-c/json-c/issues/757) - json_object_from_fd_ex: fail if file is too large
* [Issue #759](https://github.com/json-c/json-c/issues/759) - json_tokener_parse_ex: handle out of memory errors
* [Issue #766](https://github.com/json-c/json-c/issues/766) - Some people have trouble with undefined references to arc4random 
* [Issue #767](https://github.com/json-c/json-c/issues/767) - How to create a character array using json-c
* [Issue #768](https://github.com/json-c/json-c/issues/768) - commits from May 30, 2022 killed my docker build process
* [Issue #769](https://github.com/json-c/json-c/issues/769) - Issue #768
* [Issue #770](https://github.com/json-c/json-c/issues/770) - json_parse.c:170:13: error: this statement may fall through
* [Issue #771](https://github.com/json-c/json-c/issues/771) - fix fallthough warning
* [Issue #772](https://github.com/json-c/json-c/issues/772) - add JSON_C_TO_STRING_COLOR option
* [Issue #773](https://github.com/json-c/json-c/issues/773) - problem with u_int64_t
* [Issue #774](https://github.com/json-c/json-c/issues/774) - The function add_compile_options was added to CMake version 2.8.12 and later but your minimum is 2.8 which will not work
* [Issue #775](https://github.com/json-c/json-c/issues/775) - list(TRANSFORM ...) is not available prior to CMake 3.12.
* [Issue #776](https://github.com/json-c/json-c/issues/776) - Fix typo
* [Issue #777](https://github.com/json-c/json-c/issues/777) - Don't try to change locale when libc only supports the C locale
* [Issue #778](https://github.com/json-c/json-c/issues/778) - Do not insert newlines when converting empty arrays to json string and JSON_C_TO_STRING_PRETTY is used
* [Issue #779](https://github.com/json-c/json-c/issues/779) - Fix compiling for Android
* [Issue #780](https://github.com/json-c/json-c/issues/780) - Memory Leak when setting empty strings when c_string.pdata is used
* [Issue #781](https://github.com/json-c/json-c/issues/781) - Fix memory leak with emtpy strings in json_object_set_string
* [Issue #782](https://github.com/json-c/json-c/issues/782) - Fix typos found by codespell
* [Issue #783](https://github.com/json-c/json-c/issues/783) - Fix build with clang-15+
* [Issue #784](https://github.com/json-c/json-c/issues/784) - get_time_seed(): silence warning emitted by Coverity Scan static analyzer
* [Issue #786](https://github.com/json-c/json-c/issues/786) - ghpages update was not published for json-c-0.16
* [Issue #787](https://github.com/json-c/json-c/issues/787) - -static linker flag result in building failed
* [Issue #788](https://github.com/json-c/json-c/issues/788) - Clear sensitive information.
* [Issue #789](https://github.com/json-c/json-c/issues/789) - Unnecessary struct declaration and unsafe function usage
* [Issue #790](https://github.com/json-c/json-c/issues/790) - Small update to README file
* [Issue #791](https://github.com/json-c/json-c/issues/791) - json_object_object_foreach not ISO-C compliant
* [Issue #792](https://github.com/json-c/json-c/issues/792) - ` json_object_get_int` does not set `EINVAL` on invalid string
* [Issue #794](https://github.com/json-c/json-c/issues/794) - replaced
* [Issue #796](https://github.com/json-c/json-c/issues/796) - Added Test for get int functions
* [Issue #797](https://github.com/json-c/json-c/issues/797) - make uninstall
* [Issue #798](https://github.com/json-c/json-c/issues/798) - API to deal with enums is missing
* [Issue #799](https://github.com/json-c/json-c/issues/799) - json_object_put: Assertion `jso->_ref_count > 0' failed.
* [Issue #800](https://github.com/json-c/json-c/issues/800) - String converted to scientific notation
* [Issue #801](https://github.com/json-c/json-c/issues/801) - #error You do not have strncasecmp on your system.
* [Issue #802](https://github.com/json-c/json-c/issues/802) - Problem: modern CMake warns about version 2.8
* [Issue #803](https://github.com/json-c/json-c/issues/803) - Problem: confusing error message in snprintf_compat.h
* [Issue #804](https://github.com/json-c/json-c/issues/804) - Problem: cmake 3.25.1 warns about CMP0042 not being set
* [Issue #806](https://github.com/json-c/json-c/issues/806) - The problem is libjson-c.dylib incompatible with OS version
* [Issue #807](https://github.com/json-c/json-c/issues/807) - json simple parse syntax
* [Issue #808](https://github.com/json-c/json-c/issues/808) - iOS Build using cmake fails due to 64 to 32bits conversion precision loss
* [Issue #809](https://github.com/json-c/json-c/issues/809) - Feature request json_object_new_uint() 
* [Issue #810](https://github.com/json-c/json-c/issues/810) - docs: update to Internet Standard reference
* [Issue #811](https://github.com/json-c/json-c/issues/811) - dependence on execution character set
* [Issue #812](https://github.com/json-c/json-c/issues/812) - Duplicate symbol when compiling with clang-cl
* [Issue #813](https://github.com/json-c/json-c/issues/813) - Build apps only in project itself.
* [Issue #814](https://github.com/json-c/json-c/issues/814) - Code execution order
* [Issue #816](https://github.com/json-c/json-c/issues/816) - Hi I need to generate libjson-c.so.3 and libjson-c.so.3.0.1, please help with steps
* [Issue #818](https://github.com/json-c/json-c/issues/818) - error: a function declaration without a prototype is deprecated in all versions of C
* [Issue #819](https://github.com/json-c/json-c/issues/819) - build with intel 2023 fails on vasprintf
* [Issue #820](https://github.com/json-c/json-c/issues/820) - ISO C forbids in 
* [Issue #821](https://github.com/json-c/json-c/issues/821) - Any release planing for 0.17?
* [Issue #822](https://github.com/json-c/json-c/issues/822) - Added option to disable app build
* [Issue #823](https://github.com/json-c/json-c/issues/823) - Symbol not found during linking stage of libjson-c.so
