# How to use cppsuite
This README demonstrates the process to create and run a cppsuite test from scratch. As an example, we will create a new test `multi_size_inserts` that performs transactions that are either within or greater than the database cache size.  
We will cover the steps to [create your new test](#creating-a-new-test), [configure it](#configuring-tests), and finally [run the test](#running-tests).

## Creating a new test
New tests are created using the [create_test.sh](./create_test.sh) script in the root directory. This script takes the test name as its only argument, so we will call it with:  
`./create_test.sh multi_size_inserts`  

This will create two new files: the `.cpp` file `tests/multi_size_inserts.cpp` and the configuration file `configs/multi_size_inserts_default.txt`. Working with these files is covered below.  

*Note: `s_all` is called as part of `create_test.sh` which automatically updates some boilerplate sections of the test framework based on the contents of `test_data.py`. If you want to change the name of an existing test you will need to also update it in `test_data.py` and then call `s_all` manually.*

## Configuring tests
Having generated these two files we now need to modify them to define our test.

### The `.cpp` file

The `.cpp` file allows users to define the behaviour of database operations for their test. This file is cloned from [test_template.cpp](tests/test_template.cpp) on creation and by default will do nothing as all functions are stubbed.

#### Defining workloads
In our example test `multi_size_inserts` we will first update the functions that define `insert` and `populate`.
The default behavior of populate (implemented in [database_operation.cpp](./src/main/database_operation.cpp)) will create the table in the database, so we can delete the stubbed implementation of `populate` and make sure that it is defined in the [configuration file](#the-configuration-file) later.
When cppsuite executes the test it will create threads that run the `insert_operation` function.  

Initially, the insert function is stubbed as follows:
```cpp
void
insert_operation(thread_worker *) override final
{
    logger::log_msg(LOG_WARN, "insert_operation: nothing done");
}
```
The user can either define their own insertion logic by filling out the function body, or remove the `insert_operation` stub to fall back on the framework’s default implementation.
For our `multi_size_inserts` example, we add code to to write transactions with either 10MB or 1KB updates. We will configure the `cache_size` to be 10MB later.

```cpp
void
insert_operation(thread_worker *tw) override final
{
    const int64_t ten_mb = 10 * 1024 * 1024;
    const int64_t one_kb = 1024;

    // Retrieve the collection created by the populate operation.
    collection &coll = tw->db.get_collection(0);
    scoped_cursor cursor = tw->session.open_scoped_cursor(coll.name);

    int value_size_bytes = one_kb;

    while (tw->running()) {

        // Swap value size between 1KB and 10MB.
        value_size_bytes = value_size_bytes == one_kb ? ten_mb : one_kb;

        tw->txn.begin();

        bool success = true;
        // Each transaction should have target_op_count insert operations. This value is set in the configuration file
        for(int i = 0; i < tw->txn.get_target_op_count(); ++i) {
            tw->sleep();
    
            // Populate the key and value with random strings. We only use the size of the value in this test
            const std::string key = random_generator::instance().generate_pseudo_random_string(tw->key_size);
            const std::string value = random_generator::instance().generate_pseudo_random_string(value_size_bytes);

            if(!tw->insert(cursor, coll.id, key, value)) {
                success = false;
                break;
            }
        }

        if(!success){
            tw->txn.rollback();
        } else {
            testutil_assert(tw->txn.commit());
        }
    }
}
```


#### Saving validation data
The `set_tracking_cursor` function defines what data is saved by the operation tracker. 
In our example test we will save the timestamp, transaction ID, key and value size.

```cpp
void
set_tracking_cursor(WT_SESSION *session, const tracking_operation &operation, const uint64_t &collection_id,
    const std::string &key, const std::string &value, wt_timestamp_t ts,
    scoped_cursor &op_track_cursor) override final
{
    uint64_t txn_id = reinterpret_cast<WT_SESSION_IMPL *>(session)->txn->id;

    op_track_cursor->set_key(op_track_cursor.get(), ts, txn_id);
    op_track_cursor->set_value(op_track_cursor.get(), key.c_str(), value.size());
}
```
*Note: We also need to specify the key-value format of the tracking table where we save test data. This is covered [below](#the-configuration-file).*

##### Performing validation
When the test finishes the framework calls the `validate` function in which users can perform any checks they want. 

In our test `multi_size_inserts` the 10MB updates are too large for cache and not be written ([see below](#the-configuration-file) for how to configure cache size). As such we will check that any update made with a value size of 1KB is present in the database, and any update with a value size of 10MB is not present:

```cpp
void
validate(const std::string &operation_table_name, const std::string &schema_table_name,
    database &db) override final
{
    const int64_t ten_mb = 10 * 1024 * 1024;
    const int64_t one_kb = 1024;

    scoped_session session = connection_manager::instance().create_session();

    // For this test we only need to check the insert operations data saved by the operation tracker.
    scoped_cursor tracked_operations_cursor = session.open_scoped_cursor(operation_table_name);
    testutil_assert(!db.get_collection_ids().empty());
    scoped_cursor cursor = session.open_scoped_cursor(db.get_collection(0).name);

    while (tracked_operations_cursor->next(tracked_operations_cursor.get()) == 0) {
        int ts = 0, txn_id = 0, value_size = 0;
        char *key = nullptr;

        testutil_check(tracked_operations_cursor->get_key(tracked_operations_cursor.get(), &ts, &txn_id));
        testutil_check(tracked_operations_cursor->get_value(tracked_operations_cursor.get(), &key, &value_size));

        testutil_assert(value_size == one_kb || value_size == ten_mb);
        cursor->set_key(cursor.get(), key);
        if(value_size == one_kb) {
            testutil_assert(cursor->search(cursor.get()) == 0);
        } else if(value_size == ten_mb) {
            testutil_assert(cursor->search(cursor.get()) == WT_NOTFOUND);
        }
    }
}
```

### The configuration file

The configuration file manages cppsuite level configuration for the test such as the total runtime of the test and the behaviour of the different components in the test. To see what fields can be set please refer to [test_data.py](../../dist/test_data.py).  
In our `multi_size_inserts_default.txt` file we define our test to run for 15 seconds, have a cache of 10MB, and create 10 insert threads. Each thread runs `insert_operation` where each transaction contains between 5 and 10 inserts and performs one insert every 10 milliseconds.
We define the operation tracker key and value formats to match the types of the data we save in `set_tracking_cursor` [above](#saving-validation-data). The types that can be used are found [here](https://source.wiredtiger.com/develop/schema.html#schema_format_types).

```txt
duration_seconds=15,
cache_size_mb=10,

workload_manager=
(
    populate_config=
    (
        collection_count=1,
    ),

    insert_config=
    (
        thread_count=1,
        op_rate=10ms,
        ops_per_transaction=(min=5,max=10),
    ),
),

operation_tracker=
(
    tracking_key_format=QQ,
    tracking_value_format=SQ
)
```

#### Multiple configuration files
Sometimes users may want to use the same test workload but run it at different intensities to stress different aspects of WiredTiger. To do so users may define multiple configuration files. As a simple example we can create a second configuration file `multi_size_inserts_stress.txt` that is identical to the original configuration above, but changes `op_rate` to 1ms.


## Running tests
Having written our test we can now run it. Compile WiredTiger and then navigate to the `<build_dir>/test/cppsuite` folder.  

The `./run` binary executes the test. Further details are provided via the `-h` help flag, but the most common usage is 
`./run -t multi_size_inserts -f configs/multi_size_inserts_default.txt -l 2`.  

In this command the `-t` flag tells cppsuite to use the `test/multi_size_inserts.cpp` test configuration, the `-f` flag to use the `configs/multi_size_inserts_default.txt` cppsuite configuration, and the `-l` flag to run the test with level 2 verbosity. Further details on log levels can be [found here](./README.md#logging).

**Warning: If you change your configuration file you will need to recompile WiredTiger to have the changes propagate into the build folder. Make sure you update the file in the `test/cppsuite` folder and not the `<build_dir>/test/cppsuite` folder as the latter will be overwritten when WiredTiger is recompiled.**
