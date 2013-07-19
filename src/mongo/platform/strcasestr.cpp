/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/strcasestr.h"

#if defined(__sunos__)
#include <dlfcn.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#endif

#if defined(_WIN32) || defined(__sunos__)

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

#if defined(__sunos__)
#define STRCASESTR_EMULATION_NAME strcasestr_emulation
#else
#define STRCASESTR_EMULATION_NAME strcasestr
#endif

namespace mongo {
namespace pal {

    /**
     * strcasestr -- case-insensitive search for a substring within another string.
     *
     * @param haystack      ptr to C-string to search
     * @param needle        ptr to C-string to try to find within 'haystack'
     * @return              ptr to start of 'needle' within 'haystack' if found, NULL otherwise
     */
    const char* STRCASESTR_EMULATION_NAME(const char* haystack, const char* needle) {

        std::string haystackLower(haystack);
        std::transform(haystackLower.begin(),
                       haystackLower.end(),
                       haystackLower.begin(),
                       ::tolower);

        std::string needleLower(needle);
        std::transform(needleLower.begin(),
                       needleLower.end(),
                       needleLower.begin(),
                       ::tolower);

        // Use strstr() to find 'lowercased needle' in 'lowercased haystack'
        // If found, use the location to compute the matching location in the original string
        // If not found, return NULL
        const char* haystackLowerStart = haystackLower.c_str();
        const char* location = strstr(haystackLowerStart, needleLower.c_str());
        return location ? (haystack + (location - haystackLowerStart)) : NULL;
    }

#if defined(__sunos__)

    typedef const char* (*StrCaseStrFunc)(const char* haystack, const char* needle);
    static StrCaseStrFunc strcasestr_switcher = mongo::pal::strcasestr_emulation;

    const char* strcasestr(const char* haystack, const char* needle) {
        return strcasestr_switcher(haystack, needle);
    }

#endif // #if defined(__sunos__)

} // namespace pal
} // namespace mongo

#endif // #if defined(_WIN32) || defined(__sunos__)

#if defined(__sunos__)

namespace mongo {

    // 'strcasestr()' on Solaris will call the emulation if the symbol is not found
    //
    MONGO_INITIALIZER_GENERAL(SolarisStrCaseCmp,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {
        void* functionAddress = dlsym(RTLD_DEFAULT, "strcasestr");
        if (functionAddress != NULL) {
            mongo::pal::strcasestr_switcher =
                    reinterpret_cast<mongo::pal::StrCaseStrFunc>(functionAddress);
        }
        return Status::OK();
    }

} // namespace mongo

#endif // __sunos__
