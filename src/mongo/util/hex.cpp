// util/hex.cpp

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

#include <string>
#include "mongo/util/hex.h"

namespace mongo {

    template<typename T>
    std::string integerToHexDef(T inInt) {
        if(!inInt) 
            return "0";

        static const char hexchars[] = "0123456789ABCDEF";

        static const size_t outbufSize = sizeof(T) * 2 + 1;
        char outbuf[outbufSize];
        outbuf[outbufSize - 1] = '\0';

        char c;
        int lastSeenNumber = 0;
        for (int j = int(outbufSize) - 2; j >= 0; j--) {
            c = hexchars[inInt & 0xF];
            if(c != '0')
                lastSeenNumber = j;
            outbuf[j] = c;
            inInt = inInt >> 4;
        }
        char *bufPtr = outbuf;
        bufPtr += lastSeenNumber;

        return std::string(bufPtr);
    }

    template<> std::string integerToHex<int>(int val) { return integerToHexDef(val); }
    template<> std::string integerToHex<unsigned int>(unsigned int val) { 
        return integerToHexDef(val); }
    template<> std::string integerToHex<long>(long val) { return integerToHexDef(val); }
    template<> std::string integerToHex<long long>(long long val) { return integerToHexDef(val); }
}
