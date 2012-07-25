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

	class Base {
    public:
        Base( const std::string& name )
        	: _ns(name) {
        }

        virtual ~Base() {
        }

        const char * ns() const { return _ns.c_str(); }
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

			myStream.str("");
			myStream << y;
			ASSERT_EQUALS("{ x: \"asdf\", y: true, subobj: { z: 3, q: 4 } }", myStream.str());
			ASSERT_EQUALS(3, y.getFieldDotted("subobj.z").Number());
			ASSERT_EQUALS(3, y["subobj"]["z"].Number());

			/* fetch all *top level* elements from object y into a vector */
			std::vector<mongo::BSONElement> myVector;
			y.elems(myVector);

			ASSERT_EQUALS(3u, myVector.size());

			myStream.str("");
			myStream << myVector[0];
			ASSERT_EQUALS("x: \"asdf\"", myStream.str());
			ASSERT_EQUALS("asdf", myVector[0].String());
			ASSERT_TRUE(myVector[1].Bool());
			ASSERT_EQUALS("subobj: { z: 3, q: 4 }", myVector[2].toString());

			/* into an array */
			typedef std::list<mongo::BSONElement> BSONElementList;
			BSONElementList myList;
			y.elems(myList);

			ASSERT_EQUALS(3u, myList.size());
			BSONElementList::const_iterator myIt = myList.begin();
			myStream.str("");
			myStream << *myIt;
			ASSERT_EQUALS("x: \"asdf\"", myStream.str());
			++myIt;
			ASSERT_TRUE((*myIt).Bool());
			++myIt;
			ASSERT_EQUALS("subobj: { z: 3, q: 4 }", (*myIt).toString());

			mongo::BSONObj sub = y["subobj"].Obj();
			ASSERT_EQUALS("{ z: 3, q: 4 }", sub.toString());

			mongo::BSONObj::iterator myBSONIterator(y);
			ASSERT_TRUE(myBSONIterator.more());
			ASSERT_EQUALS("x: \"asdf\"", myBSONIterator.next().toString());
			ASSERT_TRUE(myBSONIterator.more());
			ASSERT_EQUALS("y: true", myBSONIterator.next().toString());
			ASSERT_TRUE(myBSONIterator.more());
			ASSERT_EQUALS("subobj: { z: 3, q: 4 }", myBSONIterator.next().toString());
			ASSERT_FALSE(myBSONIterator.more());
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
