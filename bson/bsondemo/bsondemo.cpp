/** @file bsondemo.cpp */

/*    Copyright 2010 10gen Inc.
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

using namespace std;
using namespace mongo;

int main()
{
    std::cout << "hello" << std::endl;

    BSONObj empty;
    cout << empty.toString() << endl;

    BSONObj x = BSONObjBuilder().append("name", "joe").append("age", 33.7).obj();
    cout << x.toString() << endl;
    cout << x["name"].String() << ' ' << x["age"].Number() << ' ' << x.isEmpty() << endl;

    BSONObj y = BSON( "x" << "asdf" << "y" << true << "subobj" << BSON( "z" << 3 ) );
    cout << y.toString() << endl;
    cout << y.getFieldDotted("subobj.z").Number() << endl;
 
	return 0;
}
