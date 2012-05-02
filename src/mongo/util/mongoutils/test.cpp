/* @file test.cpp
  utils/mongoutils/test.cpp
  unit tests for mongoutils
*/

/*
 *    Copyright 2010 10gen Inc.
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

#include <assert.h>
#include "str.h"
#include "html.h"

using namespace std;
using namespace mongoutils;

int main() {
    {
        string s = "abcde";
        str::stripTrailing(s, "ef");
        verify( s == "abcd" );
        str::stripTrailing(s, "abcd");
        verify( s.empty() );
        s = "abcddd";
        str::stripTrailing(s, "d");
        verify( s == "abc" );
    }

    string x = str::after("abcde", 'c');
    verify( x == "de" );
    verify( str::after("abcde", 'x') == "" );
    return 0;
}
