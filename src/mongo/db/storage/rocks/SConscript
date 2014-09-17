Import("env")
Import("has_option")

if has_option("rocksdb"):

    env.Library(
        target= 'storage_rocks_base',
        source= [
            'rocks_collection_catalog_entry.cpp',
            'rocks_database_catalog_entry.cpp',
            'rocks_engine.cpp',
            'rocks_record_store.cpp',
            'rocks_recovery_unit.cpp',
            'rocks_sorted_data_impl.cpp',
            ],
        LIBDEPS= [
            '$BUILD_DIR/mongo/bson',
            '$BUILD_DIR/mongo/db/catalog/collection_options',
            '$BUILD_DIR/mongo/db/index/index_descriptor',
            '$BUILD_DIR/mongo/db/storage/bson_collection_catalog_entry',
            '$BUILD_DIR/mongo/db/storage/index_entry_comparison',
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
        target='storage_rocks_sorted_data_impl_test',
        source=['rocks_sorted_data_impl_test.cpp',
                ],
        LIBDEPS=[
            'storage_rocks_fake'
            ]
        )


    env.CppUnitTest(
       target='storage_rocks_sorted_data_impl_harness_test',
       source=['rocks_sorted_data_impl_harness_test.cpp'
               ],
       LIBDEPS=[
            'storage_rocks_fake',
            '$BUILD_DIR/mongo/db/storage/sorted_data_interface_test_harness'
            ]
       )
