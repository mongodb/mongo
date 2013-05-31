// stringutils_test.cpp

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

#include "mongo/unittest/unittest.h"

#include "mongo/bson/util/misc.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/hex.h"

namespace mongo {

    TEST(Comparison, Basic) {

        //
        // Basic version comparison tests with different version string types
        //

        // Equal
        ASSERT(versionCmp("1.2.3", "1.2.3") == 0);

        // Basic
        ASSERT(versionCmp("1.2.3", "1.2.4") < 0);
        ASSERT(versionCmp("1.2.3", "1.2.20") < 0);
        ASSERT(versionCmp("1.2.3", "1.20.3") < 0);
        ASSERT(versionCmp("2.2.3", "10.2.3") < 0);

        // Post-fixed
        ASSERT(versionCmp("1.2.3", "1.2.3-") > 0);
        ASSERT(versionCmp("1.2.3", "1.2.3-pre") > 0);
        ASSERT(versionCmp("1.2.3", "1.2.4-") < 0);
        ASSERT(versionCmp("1.2.3-", "1.2.3") < 0);
        ASSERT(versionCmp("1.2.3-pre", "1.2.3") < 0);
    }

    TEST( LexNumCmp, Simple1 ) {
        ASSERT_EQUALS( 0, LexNumCmp::cmp( "a.b.c", "a.b.c", false ) );
    }

    void assertCmp( int expected,
                    const StringData& s1,
                    const StringData& s2,
                    bool lexOnly = false ) {
        mongo::LexNumCmp cmp( lexOnly );
        ASSERT_EQUALS( expected, cmp.cmp( s1, s2, lexOnly ) );
        ASSERT_EQUALS( expected, cmp.cmp( s1, s2 ) );
        ASSERT_EQUALS( expected < 0, cmp( s1, s2 ) );
    }
    
    TEST( LexNumCmp, Simple2 ) {
        ASSERT( ! isdigit( (char)255 ) );

        assertCmp( 0, "a", "a" );
        assertCmp( -1, "a", "aa" );
        assertCmp( 1, "aa", "a" );
        assertCmp( -1, "a", "b" );
        assertCmp( 1, "100", "50" );
        assertCmp( -1, "50", "100" );
        assertCmp( 1, "b", "a" );
        assertCmp( 0, "aa", "aa" );
        assertCmp( -1, "aa", "ab" );
        assertCmp( 1, "ab", "aa" );
        assertCmp( 1, "0", "a" );
        assertCmp( 1, "a0", "aa" );
        assertCmp( -1, "a", "0" );
        assertCmp( -1, "aa", "a0" );
        assertCmp( 0, "0", "0" );
        assertCmp( 0, "10", "10" );
        assertCmp( -1, "1", "10" );
        assertCmp( 1, "10", "1" );
        assertCmp( 1, "11", "10" );
        assertCmp( -1, "10", "11" );
        assertCmp( 1, "f11f", "f10f" );
        assertCmp( -1, "f10f", "f11f" );
        assertCmp( -1, "f11f", "f111" );
        assertCmp( 1, "f111", "f11f" );
        assertCmp( -1, "f12f", "f12g" );
        assertCmp( 1, "f12g", "f12f" );
        assertCmp( 1, "aa{", "aab" );
        assertCmp( -1, "aa{", "aa1" );
        assertCmp( -1, "a1{", "a11" );
        assertCmp( 1, "a1{a", "a1{" );
        assertCmp( -1, "a1{", "a1{a" );
        assertCmp( 1, "21", "11" );
        assertCmp( -1, "11", "21" );

        assertCmp( -1 , "a.0" , "a.1" );
        assertCmp( -1 , "a.0.b" , "a.1" );

        assertCmp( -1 , "b." , "b.|" );
        assertCmp( -1 , "b.0e" , (string("b.") + (char)255).c_str() );
        assertCmp( -1 , "b." , "b.0e" );

        assertCmp( 0, "238947219478347782934718234", "238947219478347782934718234");
        assertCmp( 0, "000238947219478347782934718234", "238947219478347782934718234");
        assertCmp( 1, "000238947219478347782934718235", "238947219478347782934718234");
        assertCmp( -1, "238947219478347782934718234", "238947219478347782934718234.1");
        assertCmp( 0, "238", "000238");
        assertCmp( 0, "002384", "0002384");
        assertCmp( 0, "00002384", "0002384");
        assertCmp( 0, "0", "0");
        assertCmp( 0, "0000", "0");
        assertCmp( 0, "0", "000");
        assertCmp( -1, "0000", "0.0");
        assertCmp( 1, "2380", "238");
        assertCmp( 1, "2385", "2384");
        assertCmp( 1, "2385", "02384");
        assertCmp( 1, "2385", "002384");
        assertCmp( -1, "123.234.4567", "00238");
        assertCmp( 0, "123.234", "00123.234");
        assertCmp( 0, "a.123.b", "a.00123.b");
        assertCmp( 1, "a.123.b", "a.b.00123.b");
        assertCmp( -1, "a.00.0", "a.0.1");
        assertCmp( 0, "01.003.02", "1.3.2");
        assertCmp( -1, "1.3.2", "10.300.20");
        assertCmp( 0, "10.300.20", "000000000000010.0000300.000000020");
        assertCmp( 0, "0000a", "0a");
        assertCmp( -1, "a", "0a");
        assertCmp( -1, "000a", "001a");
        assertCmp( 0, "010a", "0010a");
            
        assertCmp( -1 , "a0" , "a00" );
        assertCmp( 0 , "a.0" , "a.00" );
        assertCmp( -1 , "a.b.c.d0" , "a.b.c.d00" );
        assertCmp( 1 , "a.b.c.0.y" , "a.b.c.00.x" );
            
        assertCmp( -1, "a", "a-" );
        assertCmp( 1, "a-", "a" );
        assertCmp( 0, "a-", "a-" );

        assertCmp( -1, "a", "a-c" );
        assertCmp( 1, "a-c", "a" );
        assertCmp( 0, "a-c", "a-c" );

        assertCmp( 1, "a-c.t", "a.t" );
        assertCmp( -1, "a.t", "a-c.t" );
        assertCmp( 0, "a-c.t", "a-c.t" );

        assertCmp( 1, "ac.t", "a.t" );
        assertCmp( -1, "a.t", "ac.t" );
        assertCmp( 0, "ac.t", "ac.t" );            
    }
    
    TEST( LexNumCmp, LexOnly ) {
        assertCmp( -1, "0", "00", true );
        assertCmp( 1, "1", "01", true );
        assertCmp( -1, "1", "11", true );
        assertCmp( 1, "2", "11", true );
    }

    TEST( LexNumCmp, Substring1) {
        assertCmp( 0, "1234", "1234", false );
        assertCmp( 0, StringData("1234"), StringData("1234"), false );
        assertCmp( 0, StringData("1234", 4), StringData("1234", 4), false );
        assertCmp( -1, StringData("123", 3), StringData("1234", 4), false );


        assertCmp( 0, StringData("0001", 3), StringData("0000", 3), false );

    }

    TEST( IntegerToHex, VariousConversions ) {
        ASSERT_EQUALS(std::string("0"), integerToHex(0));
        ASSERT_EQUALS(std::string("1"), integerToHex(1));
        ASSERT_EQUALS(std::string("1337"), integerToHex(0x1337));
        ASSERT_EQUALS(std::string("FFFFD499"), integerToHex(-11111));
        ASSERT_EQUALS(std::string("F1FE60C4"), integerToHex(-234987324));
        ASSERT_EQUALS(std::string("80000000"), integerToHex(std::numeric_limits<int>::min()));
        ASSERT_EQUALS(std::string("7FFFFFFF"), integerToHex(std::numeric_limits<int>::max()));
        ASSERT_EQUALS(std::string("7FFFFFFFFFFFFFFF"), 
                      integerToHex(std::numeric_limits<long long>::max()));
        ASSERT_EQUALS(std::string("8000000000000000"), 
                      integerToHex(std::numeric_limits<long long>::min()));
    }
}
