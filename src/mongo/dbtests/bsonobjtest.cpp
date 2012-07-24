// cusrortests.cpp // cursor related unit tests
//

/**
 *    Copyright (C) 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dbtests.h"

namespace BSONObjTests {

	inline void iter(mongo::BSONObj o) {
		/* iterator example */
		cout << "\niter()\n";
		for( mongo::BSONObj::iterator i(o); i.more(); ) {
			cout << ' ' << i.next().toString() << '\n';
		}
	}

	class Base {
    public:
        Base( const std::string& name )
        	: _ns(name) {
        }

        virtual ~Base() {
        }

        const char * ns() { return _ns.c_str(); }
    private:
        const std::string _ns;
    };

	class Usage : public Base {
	public:
		Usage() : Base( "Usage" ) {}
		void run() {
			mongo::BSONObj empty;

			std::stringstream myStream;
			myStream << empty;
			ASSERT_EQUALS("{}", myStream.str());

			/* make a simple { name : 'joe', age : 33.7 } object */
			{
				std::stringstream myStream;

				mongo::BSONObjBuilder b;
				b.append("name", "joe");
				b.append("age", 33.7);
				myStream << b.obj();

				ASSERT_EQUALS("{ name: \"joe\", age: 33.7 }", myStream.str());
			}
			myStream.str("");

			/* make { name : 'joe', age : 33.7 } with a more compact notation. */
			mongo::BSONObj x = mongo::BSONObjBuilder().append("name", "joe").append("age", 33.7).obj();
			myStream << x;

			ASSERT_EQUALS("{ name: \"joe\", age: 33.7 }", myStream.str());

			/* convert from bson to json */
			std::string json = x.toString();
			ASSERT_EQUALS("{ name: \"joe\", age: 33.7 }", json);

			/* access some fields of bson object x */
			ASSERT_EQUALS("joe", x["name"].String());
			ASSERT_EQUALS(33.7, x["age"].Number());
			ASSERT_FALSE(x.isEmpty());

			/* make a bit more complex object with some nesting
			   { x : 'asdf', y : true, subobj : { z : 3, q : 4 } }
			*/
			mongo::BSONObj y = BSON( "x" << "asdf" << "y" << true << "subobj" << BSON( "z" << 3 << "q" << 4 ) );

			/* print it */
			cout << "y: " << y << endl;

			/* reach in and get subobj.z */
			cout << "subobj.z: " << y.getFieldDotted("subobj.z").Number() << endl;

			/* alternate syntax: */
			cout << "subobj.z: " << y["subobj"]["z"].Number() << endl;

			/* fetch all *top level* elements from object y into a vector */
			vector<mongo::BSONElement> v;
			y.elems(v);
			cout << v[0] << endl;

			/* into an array */
			list<mongo::BSONElement> L;
			y.elems(L);

			mongo::BSONObj sub = y["subobj"].Obj();

			/* grab all the int's that were in subobj.  if it had elements that were not ints, we throw an exception
			   (capital V on Vals() means exception if wrong type found
			*/
		//    vector<int> myints;
		//    sub.Vals(myints);
		//    cout << "my ints: " << myints[0] << ' ' << myints[1] << endl;

			/* grab all the string values from x.  if the field isn't of string type, just skip it --
			   lowercase v on vals() indicates skip don't throw.
			*/
		//    vector<string> strs;
		//    x.vals(strs);
		//    cout << strs.size() << " strings, first one: " << strs[0] << endl;

			iter(y);
		}
	};

	class All : public Suite {
    	public:
        All() : Suite( "BSONObjTests" ) {
        }

        void setupTests() {
        	add<Usage>();
        }
    } all;
} // namespace ClientTests
