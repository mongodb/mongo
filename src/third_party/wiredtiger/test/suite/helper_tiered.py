#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#

import datetime, inspect, os, random 

# These routines help run the various storage sources. They are required to manage
# generation of storage source specific configurations.

# Generate a storage store specific authentication token.
def get_auth_token(storage_source):
    auth_token = None
    if storage_source is 'local_store':
        # Fake a secret token.
        auth_token = "Secret"
    if storage_source is 's3_store':
        # Auth token is the AWS access key ID and the AWS secret key as comma-separated values.
        # We expect the values to have been provided through the environment variables.
        access_key = os.getenv('AWS_ACCESS_KEY_ID')
        secret_key = os.getenv('AWS_SECRET_ACCESS_KEY')
        if access_key and secret_key:
            auth_token = access_key + "," + secret_key
    return auth_token

# Get a list of buckets available for the storage source.
def get_bucket_info(storage_source):
    if storage_source is 'local_store':
        return([('objects1',''), ('objects2','')])
    if storage_source is 's3_store':
        return([('s3testext',',region=ap-southeast-2'),
                ('s3testext-us',',region=us-east-2')])
    return None

# Generate a unique object prefix for the S3 store. 
def generate_s3_prefix(test_name = ''):
    # Generates a unique prefix to be used with the object keys, eg:
    # "s3test_artefacts/python_2022-31-01-16-34-10_623843294/"
    prefix = 's3test_artefacts/python_'
    prefix += datetime.datetime.now().strftime('%Y-%m-%d-%H-%M-%S')
    # Range upto int32_max, matches that of C++'s std::default_random_engine
    prefix += '_' + str(random.randrange(1, 2147483646)) + '/'

    if test_name:
        prefix += test_name + '/'

    return prefix

# Generate a file system config for the object store.
def get_fs_config(storage_source, additional_conf = '', test_name = ''):
    # There is no local store specific configuration needed
    if storage_source is 'local_store':
        return additional_conf

    # There is not need to generate a unique prefix for local store
    if storage_source is 's3_store':
        # If the calling function has not provided a name, extract it from the stack
        if not test_name:
            test_name = inspect.stack()[1][3]
        fs_conf = 'prefix=' + generate_s3_prefix(test_name)
        fs_conf += additional_conf
        return fs_conf
    
    return None

