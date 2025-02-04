#ifndef AWS_S3_LIST_OBJECTS_H
#define AWS_S3_LIST_OBJECTS_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/s3/s3_client.h>

#include <aws/common/date_time.h>
#include <aws/common/string.h>

#include <aws/s3/private/s3_paginator.h>

/** Struct representing the file system relevant data for an object returned from a ListObjectsV2 API call. */
struct aws_s3_object_info {
    /**
     * When a delimiter is specified in the request, S3 groups the common prefixes that contain the delimiter.
     * This member is set to the prefix substring ending at the first occurrence of the specified delimiter,
     * analogous to a directory entry of a file system.
     */
    struct aws_byte_cursor prefix;
    /**
     * Prefix is not included. This is the object name for use with prefix for a call to GetObject()
     */
    struct aws_byte_cursor key;
    /**
     * Size of the object in bytes.
     */
    uint64_t size;
    /**
     * Timestamp from S3 on the latest modification, if you have a reliable clock on your machine, you COULD use this
     * to implement caching.
     */
    struct aws_date_time last_modified;
    /**
     * Etag for the object, usually an MD5 hash. you COULD also use this to implement caching.
     */
    struct aws_byte_cursor e_tag;
};

/**
 * Invoked when an object or prefix is encountered during a ListObjectsV2 API call. Return false, to immediately
 * terminate the list operation. Returning true will continue until at least the current page is iterated.
 */
typedef int(aws_s3_on_object_fn)(const struct aws_s3_object_info *info, void *user_data);

/**
 * Invoked upon the complete fetch and parsing of a page. If error_code is AWS_OP_SUCCESS and
 * aws_s3_paginator_has_more_results() returns true, you may want to call,
 * aws_s3_paginator_continue() from here to fetch the rest of the bucket contents.
 */
typedef void(aws_s3_on_object_list_finished_fn)(struct aws_s3_paginator *paginator, int error_code, void *user_data);

/**
 * Parameters for calling aws_s3_initiate_list_objects(). All values are copied out or re-seated and reference counted.
 */
struct aws_s3_list_objects_params {
    /**
     * Must not be NULL. The internal call will increment the reference count on client.
     */
    struct aws_s3_client *client;
    /**
     * Must not be empty. Name of the bucket to list.
     */
    struct aws_byte_cursor bucket_name;
    /**
     * Optional. The prefix to list. By default, this will be the root of the bucket. If you would like to start the
     * list operation at a prefix (similar to a file system directory), specify that here.
     */
    struct aws_byte_cursor prefix;
    /**
     * Optional. The prefix delimiter. By default, this is the '/' character.
     */
    struct aws_byte_cursor delimiter;
    /**
     * Optional. The continuation token for fetching the next page for ListBucketV2. You likely shouldn't set this
     * unless you have a special use case.
     */
    struct aws_byte_cursor continuation_token;
    /**
     * Must not be empty. The endpoint for the S3 bucket to hit. Can be virtual or path style.
     */
    struct aws_byte_cursor endpoint;
    /**
     * Callback to invoke on each object that's listed.
     */
    aws_s3_on_object_fn *on_object;
    /**
     * Callback to invoke when each page of the bucket listing completes.
     */
    aws_s3_on_object_list_finished_fn *on_list_finished;
    void *user_data;
};

AWS_EXTERN_C_BEGIN

/**
 * Initiates a list objects command (without executing it), and returns a paginator object to iterate the bucket with if
 * successful.
 *
 * Returns NULL on failure. Check aws_last_error() for details on the error that occurred.
 *
 * this is a reference counted object. It is returned with a reference count of 1. You must call
 * aws_s3_paginator_release() on this object when you are finished with it.
 *
 * This does not start the actual list operation. You need to call aws_s3_paginator_continue() to start
 * the operation.
 */
AWS_S3_API struct aws_s3_paginator *aws_s3_initiate_list_objects(
    struct aws_allocator *allocator,
    const struct aws_s3_list_objects_params *params);

AWS_EXTERN_C_END

#endif /* AWS_S3_LIST_OBJECTS_H */
