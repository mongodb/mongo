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

/** Unit tests for OwnedPointerVector. */

#include "mongo/base/owned_pointer_vector.h"

#include <string>
#include <vector>

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

    /** Helper class that logs its constructor argument to a static vector on destruction. */
    class DestructionLogger {
    public:
        DestructionLogger( const string& name ) :
            _name( name ) {
        }
        ~DestructionLogger() { _destroyedNames.push_back( _name ); }

        static std::vector<string>& destroyedNames() { return _destroyedNames; }

    private:
        string _name;
        static std::vector<string> _destroyedNames;
    };

    std::vector<string> DestructionLogger::_destroyedNames;

    TEST(OwnedPointerVectorTest, OwnedPointerDestroyed) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<DestructionLogger> owned;
            owned.mutableVector().push_back( new DestructionLogger( "foo" ) );
            // owned destroyed
        }
        ASSERT_EQUALS( 1U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "foo", DestructionLogger::destroyedNames()[ 0 ] );
    }

    TEST(OwnedPointerVectorTest, OwnedConstPointerDestroyed) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<const DestructionLogger> owned;
            owned.mutableVector().push_back( new DestructionLogger( "foo" ) );
            // owned destroyed
        }
        ASSERT_EQUALS( 1U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "foo", DestructionLogger::destroyedNames()[ 0 ] );
    }

    TEST(OwnedPointerVectorTest, OwnedPointersDestroyedInOrder) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<DestructionLogger> owned;
            owned.mutableVector().push_back( new DestructionLogger( "first" ) );
            owned.mutableVector().push_back( new DestructionLogger( "second" ) );
            // owned destroyed
        }
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[ 0 ] );
        ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[ 1 ] );
    }
    
} // namespace
} // namespace mongo
