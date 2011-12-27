/** @file bsondemo.cpp

    Example of use of BSON from C++.

    Requires boost (headers only).
    Works headers only (the parts actually exercised herein that is - some functions require .cpp files).

    To build and run:
      g++ -o bsondemo bsondemo.cpp
      ./bsondemo

    Windows: project files are available in this directory for bsondemo.cpp for use with Visual Studio.
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

#include "../bson.h"
#include <iostream>
#include <vector>

using namespace std;
using namespace bson;

void iter(bo o) {
    /* iterator example */
    cout << "\niter()\n";
    for( bo::iterator i(o); i.more(); ) {
        cout << ' ' << i.next().toString() << '\n';
    }
}

int main() {
    cout << "build bits: " << 8 * sizeof(char *) << '\n' <<  endl;

    /* a bson object defaults on construction to { } */
    bo empty;
    cout << "empty: " << empty << endl;

    /* make a simple { name : 'joe', age : 33.7 } object */
    {
        bob b;
        b.append("name", "joe");
        b.append("age", 33.7);
        b.obj();
    }

    /* make { name : 'joe', age : 33.7 } with a more compact notation. */
    bo x = bob().append("name", "joe").append("age", 33.7).obj();

    /* convert from bson to json */
    string json = x.toString();
    cout << "json for x:" << json << endl;

    /* access some fields of bson object x */
    cout << "Some x things: " << x["name"] << ' ' << x["age"].Number() << ' ' << x.isEmpty() << endl;

    /* make a bit more complex object with some nesting
       { x : 'asdf', y : true, subobj : { z : 3, q : 4 } }
    */
    bo y = BSON( "x" << "asdf" << "y" << true << "subobj" << BSON( "z" << 3 << "q" << 4 ) );

    /* print it */
    cout << "y: " << y << endl;

    /* reach in and get subobj.z */
    cout << "subobj.z: " << y.getFieldDotted("subobj.z").Number() << endl;

    /* alternate syntax: */
    cout << "subobj.z: " << y["subobj"]["z"].Number() << endl;

    /* fetch all *top level* elements from object y into a vector */
    vector<be> v;
    y.elems(v);
    cout << v[0] << endl;

    /* into an array */
    list<be> L;
    y.elems(L);

    bo sub = y["subobj"].Obj();

    /* grab all the int's that were in subobj.  if it had elements that were not ints, we throw an exception
       (capital V on Vals() means exception if wrong type found
    */
    vector<int> myints;
    sub.Vals(myints);
    cout << "my ints: " << myints[0] << ' ' << myints[1] << endl;

    /* grab all the string values from x.  if the field isn't of string type, just skip it --
       lowercase v on vals() indicates skip don't throw.
    */
    vector<string> strs;
    x.vals(strs);
    cout << strs.size() << " strings, first one: " << strs[0] << endl;

    iter(y);
    return 0;
}

