Import("env")
Import("has_option")

if has_option("rocksdb"):

    env.Library(
        target= 'storage_rocks_base',
        source= [
            'rocks_btree_impl.cpp',
            'rocks_collection_catalog_entry.cpp',
            'rocks_database_catalog_entry.cpp',
            'rocks_engine.cpp',
            'rocks_record_store.cpp',
            'rocks_recovery_unit.cpp',
            ],
        LIBDEPS= [
            '$BUILD_DIR/mongo/bson',
            '$BUILD_DIR/mongo/db/catalog/collection_options',
            '$BUILD_DIR/mongo/db/structure/record_store',
            '$BUILD_DIR/mongo/foundation',
            '$BUILD_DIR/third_party/shim_snappy',
            ],
        SYSLIBDEPS=["rocksdb",
                    "z",
                    "bz2"] #z and bz2 are dependencies for rocks
        )

    env.Library(
        target= 'storage_rocks',
        source= [
            'rocks_database_catalog_entry_mongod.cpp',
            'rocks_init.cpp'
            ],
        LIBDEPS= [
            'storage_rocks_base'
            ]
        )

    env.Library(
        target= 'storage_rocks_fake',
        source= [
            'rocks_database_catalog_entry_fake.cpp',
            ],
        LIBDEPS= [
            'storage_rocks_base'
            ]
        )

    env.CppUnitTest(
        target='storage_rocks_engine_test',
        source=['rocks_engine_test.cpp',
                ],
        LIBDEPS=[
            'storage_rocks_fake'
            ]
        )

    env.CppUnitTest(
        target='storage_rocks_record_store_test',
        source=['rocks_record_store_test.cpp',
                ],
        LIBDEPS=[
            'storage_rocks_fake'
            ]
        )

    env.CppUnitTest(
        target='storage_rocks_btree_impl_test',
        source=['rocks_btree_impl_test.cpp',
                ],
        LIBDEPS=[
            'storage_rocks_fake'
            ]
        )

