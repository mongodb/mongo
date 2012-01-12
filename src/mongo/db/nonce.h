// @file nonce.h

/*    Copyright 2009 10gen Inc.
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

#pragma once

namespace mongo {

    typedef unsigned long long nonce64;

    struct Security {
        Security();
        static nonce64 getNonce();
        static nonce64 getNonceDuringInit(); // use this version during global var constructors
    private:
        nonce64 _getNonce();
        nonce64 __getNonce();
        ifstream *_devrandom;
        bool _initialized;
        void init(); // can call more than once
    };

} // namespace mongo
