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
#ifndef LIBMONGODBCAPI_H
#define LIBMONGODBCAPI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libmongodbcapi_db libmongodbcapi_db;
typedef struct libmongodbcapi_client libmongodbcapi_client;

typedef enum {
    LIBMONGODB_CAPI_ERROR_UNKNOWN = -1,
    LIBMONGODB_CAPI_ERROR_SUCCESS
} libmongodbcapi_error;

/**
* Starts the database and returns a handle with the service context.
*
* @param argc
*      The number of arguments in argv
* @param argv
*      The arguments that will be passed to mongod at startup to initialize state
* @param envp
*      Environment variables that will be passed to mongod at startup to initilize state
*
* @return A pointer to a db handle or null on error
*/
libmongodbcapi_db* libmongodbcapi_db_new(int argc, const char** argv, const char** envp);

/**
* Shuts down the database
*
* @param
*       A pointer to a db handle to be destroyed
*/
void libmongodbcapi_db_destroy(libmongodbcapi_db* db);

/**
* Let the database do background work. Returns an int from the error enum
*
* @param
*      The database that has work that needs to be done
*
* @return A libmongo error code
*/
int libmongodbcapi_db_pump(libmongodbcapi_db* db);

/**
* Creates a new clienst and retuns it so the caller can do operation
* A client will be destroyed when the owning db is destroyed
*
* @param db
*      The datadase that will own this client and execute its RPC calls
*
* @return A pointer to a client or null on error
*/
libmongodbcapi_client* libmongodbcapi_db_client_new(libmongodbcapi_db* db);

/**
* Destroys a client and removes it from the db/service context
* Cannot be called after the owning db is destroyed
*
* @param client
*       A pointer to the client to be destroyed
*/
void libmongodbcapi_db_client_destroy(libmongodbcapi_client* client);

/**
* Makes an RPC call to the database
*
* @param client
*      The client that will be performing the query on the database
* @param input
*      The query to be sent to and then executed by the database
* @param input_size
*      The size (number of bytes) of the input query
* @param output
*      A pointer to a void * where the database can write the location of the output.
*      The library will manage the memory pointer to by *output.
*      @TODO document lifetime of this buffer
* @param output_size
*      A pointer to a location where this function will write the size (number of bytes)
*      of the output
*
* @return A success or error code
*/
int libmongodbcapi_db_client_wire_protocol_rpc(libmongodbcapi_client* client,
                                               const void* input,
                                               size_t input_size,
                                               void** output,
                                               size_t* output_size);
/**
* @return a per-thread value indicating the last error
*/
int libmongodbcapi_get_last_error();

#ifdef __cplusplus
}
#endif

#endif
