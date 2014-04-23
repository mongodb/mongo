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
            owned.push_back( new DestructionLogger( "foo" ) );
            // owned destroyed
        }
        ASSERT_EQUALS( 1U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "foo", DestructionLogger::destroyedNames()[ 0 ] );
    }

    TEST(OwnedPointerVectorTest, OwnedPointersDestroyedInOrder) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<DestructionLogger> owned;
            owned.push_back( new DestructionLogger( "first" ) );
            owned.push_back( new DestructionLogger( "second" ) );
            // owned destroyed
        }
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[ 0 ] );
        ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[ 1 ] );
    }

    TEST(OwnedPointerVectorTest, ClearDestroyedInOrder) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<DestructionLogger> owned;
            owned.push_back( new DestructionLogger( "first" ) );
            owned.push_back( new DestructionLogger( "second" ) );

            owned.clear();
            ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[ 0 ] );
            ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[ 1 ] );
            ASSERT_EQUALS( 0U, owned.size() );
            // owned destroyed
        }
        // no additional deletion should have occured when owned was destroyed
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
    }

    TEST(OwnedPointerVectorTest, EraseDestroysAsCalled) {
        DestructionLogger::destroyedNames().clear();
        {
            // names are order of erasure
            OwnedPointerVector<DestructionLogger> owned;
            owned.push_back( new DestructionLogger( "third" ) );
            owned.push_back( new DestructionLogger( "first" ) );
            owned.push_back( new DestructionLogger( "second" ) );
            owned.push_back( new DestructionLogger( "fourth" ) );

            ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );

            // erase "first", sliding "second" down to index 1
            owned.erase(owned.begin() + 1);
            ASSERT_EQUALS( 1U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "first", DestructionLogger::destroyedNames().back() );
            ASSERT_EQUALS( 3U, owned.size() );

            // erase "second"
            owned.erase(owned.begin() + 1);
            ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "second", DestructionLogger::destroyedNames().back() );
            ASSERT_EQUALS( 2U, owned.size() );

            // erase "third"
            owned.erase(owned.begin() + 0);
            ASSERT_EQUALS( 3U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "third", DestructionLogger::destroyedNames().back() );
            ASSERT_EQUALS( 1U, owned.size() );

            // owned destroyed
        }

        // only "four" should have been deleted when owned was destroyed
        ASSERT_EQUALS( 4U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "fourth", DestructionLogger::destroyedNames().back() );
    }

    TEST(OwnedPointerVectorTest, Accessors) {
        OwnedPointerVector<int> owned;
        ASSERT_TRUE( owned.empty() );
        ASSERT_EQUALS( 0U, owned.size() );

        owned.push_back( new int(0) );
        owned.push_back( new int(1) );
        owned.push_back( new int(2) );

        ASSERT_FALSE( owned.empty() );
        ASSERT_EQUALS( 3U, owned.size() );

        ASSERT_EQUALS( 0, *owned[0] );
        ASSERT_EQUALS( 1, *owned[1] );
        ASSERT_EQUALS( 2, *owned[2] );

        ASSERT_EQUALS( 0, *owned.front() );
        ASSERT_EQUALS( 2, *owned.back() );
    }

    TEST(OwnedPointerVectorTest, TransferConstructor) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<DestructionLogger> source;
            source.push_back( new DestructionLogger( "first" ) );
            source.push_back( new DestructionLogger( "second" ) );

            {
                OwnedPointerVector<DestructionLogger> dest(source.release());
                ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
                ASSERT_EQUALS( 0U, source.size() );
                ASSERT_EQUALS( 2U, dest.size() );
                // dest destroyed
            }
            ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[ 0 ] );
            ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[ 1 ] );

            // source destroyed
        }
        // no additional deletions
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
    }

    TEST(OwnedPointerVectorTest, TransferConstructorDoesntModifyArgument) {
        DestructionLogger::destroyedNames().clear();
        {
            std::vector<DestructionLogger*> source;
            source.push_back( new DestructionLogger( "first" ) );
            source.push_back( new DestructionLogger( "second" ) );

            {
                OwnedPointerVector<DestructionLogger> dest(source);
                ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
                ASSERT_EQUALS( 2U, source.size() );
                ASSERT_EQUALS( 2U, dest.size() );
                ASSERT( source == dest.vector() ); // can't use ASSERT_EQUALS
                // dest destroyed
            }
            ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[ 0 ] );
            ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[ 1 ] );

            ASSERT_EQUALS( 2U, source.size() );
            // source destroyed
        }
        // no additional deletions
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
    }

    TEST(OwnedPointerVectorTest, TransferAssignment) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<DestructionLogger> dest;
            {
                OwnedPointerVector<DestructionLogger> source;
                source.push_back( new DestructionLogger( "first" ) );
                source.push_back( new DestructionLogger( "second" ) );

                dest = source.release();
                ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
                ASSERT_EQUALS( 0U, source.size() );
                ASSERT_EQUALS( 2U, dest.size() );
                // source destroyed
            }

            ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( 2U, dest.size() );
            // dest destroyed
        }
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[ 0 ] );
        ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[ 1 ] );
    }

    TEST(OwnedPointerVectorTest, TransferAssignmentDoesntModifyArgument) {
        DestructionLogger::destroyedNames().clear();
        {
            OwnedPointerVector<DestructionLogger> dest;
            {
                std::vector<DestructionLogger*> source;
                source.push_back( new DestructionLogger( "first" ) );
                source.push_back( new DestructionLogger( "second" ) );

                dest = source;
                ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
                ASSERT_EQUALS( 2U, source.size() );
                ASSERT_EQUALS( 2U, dest.size() );
                ASSERT( source == dest.vector() ); // can't use ASSERT_EQUALS
                // source destroyed
            }

            ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( 2U, dest.size() );
            // dest destroyed
        }
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[ 0 ] );
        ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[ 1 ] );
    }

    TEST(OwnedPointerVectorTest, ReleaseAt) {
        DestructionLogger::destroyedNames().clear();

        boost::scoped_ptr<DestructionLogger> holder;
        {
            // names are order of deletion
            OwnedPointerVector<DestructionLogger> owned;
            owned.push_back( new DestructionLogger( "first" ) );
            owned.push_back( new DestructionLogger( "third" ) );
            owned.push_back( new DestructionLogger( "second" ) );

            ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );

            // transfer ownership of "third" to holder
            holder.reset(owned.releaseAt(1));
            ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( 3U, owned.size() );
            ASSERT_EQUALS( static_cast<DestructionLogger*>(NULL), owned[1] );

            // owned destroyed
        }
        // owned deleted "first" and "second", but not "third"
        ASSERT_EQUALS( 2U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "first", DestructionLogger::destroyedNames()[0] );
        ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[1] );

        // delete "third"
        holder.reset();
        ASSERT_EQUALS( 3U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "third", DestructionLogger::destroyedNames().back() );
    }

    TEST(OwnedPointerVectorTest, PopAndReleaseBack) {
        DestructionLogger::destroyedNames().clear();

        {
            // names are order of deletion
            OwnedPointerVector<DestructionLogger> owned;
            owned.push_back( new DestructionLogger( "second" ) );
            owned.push_back( new DestructionLogger( "third" ) );
            owned.push_back( new DestructionLogger( "first" ) );

            ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );

            {
                // transfer ownership of "third" to holder
                boost::scoped_ptr<DestructionLogger> holder(owned.popAndReleaseBack());
                ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );
                ASSERT_EQUALS( 2U, owned.size() );
                // holder destroyed
            }
            ASSERT_EQUALS( 1U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "first", DestructionLogger::destroyedNames().back() );
            // owned destroyed
        }
        // owned destructor deleted "second" and "third", but not "first"
        ASSERT_EQUALS( 3U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[1] );
        ASSERT_EQUALS( "third", DestructionLogger::destroyedNames()[2] );
    }

    TEST(OwnedPointerVectorTest, PopAndDeleteBack) {
        DestructionLogger::destroyedNames().clear();

        {
            // names are order of deletion
            OwnedPointerVector<DestructionLogger> owned;
            owned.push_back( new DestructionLogger( "second" ) );
            owned.push_back( new DestructionLogger( "third" ) );
            owned.push_back( new DestructionLogger( "first" ) );

            ASSERT_EQUALS( 0U, DestructionLogger::destroyedNames().size() );

            owned.popAndDeleteBack();
            ASSERT_EQUALS( 2U, owned.size() );
            ASSERT_EQUALS( 1U, DestructionLogger::destroyedNames().size() );
            ASSERT_EQUALS( "first", DestructionLogger::destroyedNames().back() );
            // owned destroyed
        }
        // owned destructor deleted "second" and "third", but not "first"
        ASSERT_EQUALS( 3U, DestructionLogger::destroyedNames().size() );
        ASSERT_EQUALS( "second", DestructionLogger::destroyedNames()[1] );
        ASSERT_EQUALS( "third", DestructionLogger::destroyedNames()[2] );
    }
    
} // namespace
} // namespace mongo
