// kv_dictionary_test_harness.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <algorithm>
#include <vector>

#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/kv/dictionary/kv_dictionary_test_harness.h"
#include "mongo/db/storage/kv/slice.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

    using boost::scoped_ptr;

    TEST( KVDictionary, Simple1 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<KVDictionary> db( harnessHelper->newKVDictionary() );

        {
            const Slice hi = Slice::of("hi");
            const Slice there = Slice::of("there");

            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                WriteUnitOfWork uow( opCtx.get() );
                Status status = db->insert( opCtx.get(), hi, there );
                ASSERT( status.isOK() );
                status = db->get( opCtx.get(), hi, value );
                ASSERT( status.isOK() );
                status = db->remove( opCtx.get(), hi );
                ASSERT( status.isOK() );
                status = db->get( opCtx.get(), hi, value );
                ASSERT( status.code() == ErrorCodes::NoSuchKey );
                uow.commit();
            }
        }

    }

    TEST( KVDictionary, Simple2 ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<KVDictionary> db( harnessHelper->newKVDictionary() );

        const Slice hi = Slice::of("hi");
        const Slice there = Slice::of("there");
        const Slice apple = Slice::of("apple");
        const Slice bears = Slice::of("bears");

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                Status status = db->insert( opCtx.get(), hi, there );
                ASSERT( status.isOK() );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                Status status = db->insert( opCtx.get(), apple, bears );
                ASSERT( status.isOK() );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                Status status = db->get( opCtx.get(), hi, value );
                ASSERT( status.isOK() );
                ASSERT( value.size() == 6 );
                ASSERT( string( "there" ) == string( value.data() ) );
            }

            {
                Slice value;
                Status status = db->get( opCtx.get(), apple, value );
                ASSERT( status.isOK() );
                ASSERT( value.size() == 6 );
                ASSERT( string( "bears" ) == string( value.data() ) );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                Status status = db->remove( opCtx.get(), hi );
                ASSERT( status.isOK() );
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                Status status = db->get( opCtx.get(), hi, value );
                ASSERT( status.code() == ErrorCodes::NoSuchKey );
            }

            {
                Slice value;
                Status status = db->get( opCtx.get(), apple, value );
                ASSERT( status.isOK() );
                ASSERT( value.size() == 6 );
                ASSERT( string( "bears" ) == string( value.data() ) );
            }

            {
                WriteUnitOfWork uow( opCtx.get() );
                Status status = db->remove( opCtx.get(), apple  );
                ASSERT( status.isOK() );
                uow.commit();
            }

            {
                Slice value;
                Status status = db->get( opCtx.get(), apple, value );
                ASSERT( status.code() == ErrorCodes::NoSuchKey );
            }
        }
    }

    TEST( KVDictionary, InsertSerialGetSerial ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<KVDictionary> db( harnessHelper->newKVDictionary() );

        const unsigned char nKeys = 100;
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                WriteUnitOfWork uow( opCtx.get() );
                for (unsigned char i = 0; i < nKeys; i++) {
                    const Slice slice = Slice::of(i);
                    Status status = db->insert( opCtx.get(), slice, slice );
                    ASSERT( status.isOK() );
                }
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                for (unsigned char i = 0; i < nKeys; i++) {
                    Slice value;
                    Status status = db->get( opCtx.get(), Slice::of(i), value );
                    ASSERT( status.isOK() );
                    ASSERT( value.as<unsigned char>() == i );
                }
            }
        }

    }

    static int _rng(int i) { return std::rand() % i; }

    TEST( KVDictionary, InsertRandomGetSerial ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<KVDictionary> db( harnessHelper->newKVDictionary() );

        const unsigned char nKeys = 100;
        {
            std::vector<unsigned char> keys;
            for (unsigned char i = 0; i < nKeys; i++) {
                keys.push_back(i);
            }
            std::srand(unsigned(time(0)));
            std::random_shuffle(keys.begin(), keys.end(), _rng);

            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                WriteUnitOfWork uow( opCtx.get() );
                for (unsigned char i = 0; i < nKeys; i++) {
                    const Slice slice = Slice::of(keys[i]);
                    Status status = db->insert( opCtx.get(), slice, slice );
                    ASSERT( status.isOK() );
                }
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                for (unsigned char i = 0; i < nKeys; i++) {
                    Status status = db->get( opCtx.get(), Slice::of(i), value );
                    ASSERT( status.isOK() );
                    ASSERT( value.as<unsigned char>() == i );
                }
            }
        }

    }

    TEST( KVDictionary, InsertRandomCursor ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<KVDictionary> db( harnessHelper->newKVDictionary() );

        const unsigned char nKeys = 100;
        {
            std::vector<unsigned char> keys;
            for (unsigned char i = 0; i < nKeys; i++) {
                keys.push_back(i);
            }
            std::srand(unsigned(time(0)));
            std::random_shuffle(keys.begin(), keys.end(), _rng);

            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                WriteUnitOfWork uow( opCtx.get() );
                for (unsigned char i = 0; i < nKeys; i++) {
                    const Slice slice = Slice::of(keys[i]);
                    Status status = db->insert( opCtx.get(), slice, slice );
                    ASSERT( status.isOK() );
                }
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                const int direction = 1;
                unsigned char i = 0;
                for (scoped_ptr<KVDictionary::Cursor> c(db->getCursor(opCtx.get(), direction));
                     c->ok(); c->advance(), i++) {
                    ASSERT( c->currKey().as<unsigned char>() == i );
                    ASSERT( c->currVal().as<unsigned char>() == i );
                }
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                const int direction = -1;
                unsigned char i = nKeys - 1;
                for (scoped_ptr<KVDictionary::Cursor> c(db->getCursor(opCtx.get(), direction));
                     c->ok(); c->advance(), i--) {
                    ASSERT( c->currKey().as<unsigned char>() == i );
                    ASSERT( c->currVal().as<unsigned char>() == i );
                }
            }
        }

    }

    TEST( KVDictionary, InsertDeleteCursor ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<KVDictionary> db( harnessHelper->newKVDictionary() );

        const unsigned char nKeys = 100;
        std::vector<unsigned char> keys;
        for (unsigned char i = 0; i < nKeys; i++) {
            keys.push_back(i);
        }
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                WriteUnitOfWork uow( opCtx.get() );
                for (unsigned char i = 0; i < nKeys; i++) {
                    const Slice slice = Slice::of(keys[i]);
                    Status status = db->insert( opCtx.get(), slice, slice );
                    ASSERT( status.isOK() );
                }
                uow.commit();
            }
        }

        std::srand(unsigned(time(0)));
        std::random_shuffle(keys.begin(), keys.end(), _rng);

        std::set<unsigned char> remainingKeys;
        std::set<unsigned char> deletedKeys;

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                WriteUnitOfWork uow( opCtx.get() );
                for (unsigned char i = 0; i < nKeys; i++) {
                    unsigned char k = keys[i];
                    if (i < (nKeys / 2)) {
                        Status status = db->remove( opCtx.get(), Slice::of(k) );
                        ASSERT( status.isOK() );
                        deletedKeys.insert(k);
                    } else {
                        remainingKeys.insert(k);
                    }
                }
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                const int direction = 1;
                unsigned char i = 0;
                for (scoped_ptr<KVDictionary::Cursor> c(db->getCursor(opCtx.get(), direction));
                     c->ok(); c->advance(), i++) {
                    unsigned char k = c->currKey().as<unsigned char>();
                    ASSERT( remainingKeys.count(k) == 1 );
                    ASSERT( deletedKeys.count(k) == 0 );
                    ASSERT( k == c->currVal().as<unsigned char>()  );
                    remainingKeys.erase(k);
                }
                ASSERT( remainingKeys.empty() );
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                for (std::set<unsigned char>::const_iterator it = deletedKeys.begin();
                     it != deletedKeys.end(); it++) {
                    unsigned char k = *it;
                    Slice value;
                    Status status = db->get( opCtx.get(), Slice::of(k), value );
                    ASSERT( status.code() == ErrorCodes::NoSuchKey );
                    ASSERT( value.size() == 0 );
                }
            }
        }

    }

    TEST( KVDictionary, CursorSeekForward ) {
        scoped_ptr<HarnessHelper> harnessHelper( newHarnessHelper() );
        scoped_ptr<KVDictionary> db( harnessHelper->newKVDictionary() );

        const unsigned char nKeys = 101; // even number makes the test magic more complicated
        std::vector<unsigned char> keys;
        for (unsigned char i = 0; i < nKeys; i++) {
            keys.push_back(i);
        }
        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                Slice value;
                WriteUnitOfWork uow( opCtx.get() );
                for (unsigned char i = 0; i < nKeys; i += 2) {
                    const Slice slice = Slice::of(keys[i]);
                    Status status = db->insert( opCtx.get(), slice, slice );
                    ASSERT( status.isOK() );
                }
                uow.commit();
            }
        }

        {
            scoped_ptr<OperationContext> opCtx( harnessHelper->newOperationContext() );
            {
                scoped_ptr<KVDictionary::Cursor> cursor( db->getCursor( opCtx.get(), 1 ) );
                for (unsigned char i = 0; i < nKeys; i++) {
                    cursor->seek( Slice::of(keys[i]) );
                    if ( i % 2 == 0 ) {
                        ASSERT( cursor->currKey().as<unsigned char>() == i );
                    } else if ( i + 1 < nKeys ) {
                        ASSERT( cursor->currKey().as<unsigned char>() == i + 1);
                    }
                }
            }
            {
                scoped_ptr<KVDictionary::Cursor> cursor( db->getCursor( opCtx.get(), -1 ) );
                for (unsigned char i = 1; i < nKeys; i++) {
                    cursor->seek(Slice::of(keys[i]));
                    if ( i % 2 == 0 ) {
                        ASSERT( cursor->currKey().as<unsigned char>() == i );
                    } else {
                        ASSERT( cursor->currKey().as<unsigned char>() == i - 1 );
                    }
                }
            }
        }

    }

}
