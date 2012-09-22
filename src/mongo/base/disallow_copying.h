/*    Copyright 2012 10gen Inc.
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

/**
 * Instruct the compiler not to create default copy constructor and assignment operator
 * for class "CLASS".  Must be the _first_ or _last_ line of the class declaration.  Prefer
 * to use it as the first line.
 *
 * Usage:
 *    class Foo {
 *        MONGO_DISALLOW_COPYING(Foo);
 *    public:
 *        ...
 *    };
 */
#define MONGO_DISALLOW_COPYING(CLASS) \
    private:                                    \
    CLASS(const CLASS&);                        \
    CLASS& operator=(const CLASS&)
