/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/client/embedded/functions_for_test.h"

#include <mongoc.h>
#include <stdio.h>

#include "mongo/client/embedded/embedded_transport_layer.h"
#include "mongo/util/log.h"

/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool mongo::embeddedTest::insert_data(mongoc_collection_t* collection) {
    mongoc_bulk_operation_t* bulk;
    const int ndocs = 4;
    bson_t* docs[ndocs];

    bulk = mongoc_collection_create_bulk_operation(collection, true, NULL);

    docs[0] = BCON_NEW("x", BCON_DOUBLE(1.0), "tags", "[", "dog", "cat", "]");
    docs[1] = BCON_NEW("x", BCON_DOUBLE(2.0), "tags", "[", "cat", "]");
    docs[2] = BCON_NEW("x", BCON_DOUBLE(2.0), "tags", "[", "mouse", "cat", "dog", "]");
    docs[3] = BCON_NEW("x", BCON_DOUBLE(3.0), "tags", "[", "]");

    for (int i = 0; i < ndocs; i++) {
        mongoc_bulk_operation_insert(bulk, docs[i]);
        bson_destroy(docs[i]);
        docs[i] = NULL;
    }

    bson_error_t error;
    bool ret = mongoc_bulk_operation_execute(bulk, NULL, &error);

    if (!ret) {
        log() << "Error inserting data: " << error.message;
    }

    mongoc_bulk_operation_destroy(bulk);
    return ret;
}

/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool copydb(mongoc_client_t* client, const char* other_host_and_port) {
    mongoc_database_t* admindb;
    bson_t* command;
    bson_t reply;
    bson_error_t error;
    bool res;

    BSON_ASSERT(other_host_and_port);
    /* Must do this from the admin db */
    admindb = mongoc_client_get_database(client, "admin");

    command = BCON_NEW("copydb",
                       BCON_INT32(1),
                       "fromdb",
                       BCON_UTF8("test"),
                       "todb",
                       BCON_UTF8("test2"),

                       /* If you want from a different host */
                       "fromhost",
                       BCON_UTF8(other_host_and_port));
    res = mongoc_database_command_simple(admindb, command, NULL, &reply, &error);
    if (!res) {
        mongo::log() << "Error with copydb: " << error.message;
        goto copy_cleanup;
    }


copy_cleanup:
    bson_destroy(&reply);
    bson_destroy(command);
    mongoc_database_destroy(admindb);

    return res;
}
/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool clone_collection(mongoc_database_t* database, const char* other_host_and_port) {
    bson_t* command;
    bson_t reply;
    bson_error_t error;
    bool res;

    BSON_ASSERT(other_host_and_port);
    command = BCON_NEW("cloneCollection",
                       BCON_UTF8("test.remoteThings"),
                       "from",
                       BCON_UTF8(other_host_and_port),
                       "query",
                       "{",
                       "x",
                       BCON_INT32(1),
                       "}");
    res = mongoc_database_command_simple(database, command, NULL, &reply, &error);
    if (!res) {
        mongo::log() << "Error with clone: " << error.message;
        goto clone_cleanup;
    }


clone_cleanup:
    bson_destroy(&reply);
    bson_destroy(command);

    return res;
}
/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
bool mongo::embeddedTest::explain(mongoc_collection_t* collection) {

    bson_t* command;
    bson_t reply;
    bson_error_t error;
    bool res;

    command = BCON_NEW("explain",
                       "{",
                       "find",
                       BCON_UTF8((const char*)"things"),
                       "filter",
                       "{",
                       "x",
                       BCON_INT32(1),
                       "}",
                       "}");
    res = mongoc_collection_command_simple(collection, command, NULL, &reply, &error);
    if (!res) {
        log() << "Error with explain: " << error.message;
        goto explain_cleanup;
    }


explain_cleanup:
    bson_destroy(&reply);
    bson_destroy(command);
    return res;
}
/**
 * WARNING: This function is an example lifted directly from the C driver
 * for use testing the connection to the c driver. It is written in C,
 * and should not be used for anything besides basic testing.
 */
int mongo::embeddedTest::run_c_driver_all() {
    mongoc_database_t* database = NULL;
    mongoc_client_t* client = NULL;
    mongoc_collection_t* collection = NULL;
    char* host_and_port = NULL;
    int res = 0;
    char* other_host_and_port = NULL;


    client = mongoc_client_new(NULL);

    if (!client) {
        log() << " Invalid client ";
        res = 2;
        goto cleanup;
    }

    mongoc_client_set_error_api(client, 2);
    database = mongoc_client_get_database(client, "test");
    collection = mongoc_database_get_collection(database, (const char*)"things");

    if (!mongo::embeddedTest::insert_data(collection)) {
        res = 3;
        goto cleanup;
    }

    if (!mongo::embeddedTest::explain(collection)) {
        res = 4;
        goto cleanup;
    }

    if (other_host_and_port) {
        if (!copydb(client, other_host_and_port)) {
            res = 5;
            goto cleanup;
        }

        if (!clone_collection(database, other_host_and_port)) {
            res = 6;
            goto cleanup;
        }
    }

cleanup:
    if (collection) {
        mongoc_collection_destroy(collection);
    }

    if (database) {
        mongoc_database_destroy(database);
    }

    if (client) {
        mongoc_client_destroy(client);
    }

    bson_free(host_and_port);
    mongoc_cleanup();
    return res;
}
