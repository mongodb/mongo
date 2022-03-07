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
    if storage_source == 'local_store':
        # Fake a secret token.
        auth_token = "Secret"
    if storage_source == 's3_store':
        # Auth token is the AWS access key ID and the AWS secret key as semi-colon separated values.
        # We expect the values to have been provided through the environment variables.
        access_key = os.getenv('AWS_ACCESS_KEY_ID')
        secret_key = os.getenv('AWS_SECRET_ACCESS_KEY')
        if access_key and secret_key:
            auth_token = access_key + ";" + secret_key
    return auth_token

# Get buckets configured for the storage source

# S3 buckets with their regions
s3_buckets = ['s3testext;ap-southeast-2', 's3testext-us;us-east-2']

# Local buckets do not have a region
local_buckets = ['bucket1', 'bucket2']

# Get name of the first bucket in the list.
def get_bucket1_name(storage_source):
    if storage_source == 's3_store':
        return s3_buckets[0]
    if storage_source == 'local_store':
        return local_buckets[0]
    return None

# Get name of the second bucket in the list.
def get_bucket2_name(storage_source):
    if storage_source == 's3_store':
        return s3_buckets[1]
    if storage_source == 'local_store':
        return local_buckets[1]
    return None

# Generate a unique object prefix for the S3 store. 
def generate_s3_prefix(test_name = ''):
    # Generates a unique prefix to be used with the object keys, eg:
    # "s3test_artefacts/python_2022-31-01-16-34-10_623843294/"
    prefix = 's3test_artefacts--python_'
    prefix += datetime.datetime.now().strftime('%Y-%m-%d-%H-%M-%S')
    # Range upto int32_max, matches that of C++'s std::default_random_engine
    prefix += '_' + str(random.randrange(1, 2147483646)) + '--'

    # If the calling function has not provided a name, extract it from the stack.
    # It is important to generate unique prefixes for different tests in the same class,
    # so that the database namespace do not collide.
    # 0th element on the stack is the current function. 1st element is the calling function.
    if not test_name:
        test_name = inspect.stack()[1][3]
    prefix += test_name + '--'

    return prefix

