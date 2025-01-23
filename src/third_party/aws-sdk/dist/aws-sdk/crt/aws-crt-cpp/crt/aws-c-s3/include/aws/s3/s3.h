#ifndef AWS_S3_H
#define AWS_S3_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/io/logging.h>
#include <aws/s3/exports.h>

AWS_PUSH_SANE_WARNING_LEVEL

#define AWS_C_S3_PACKAGE_ID 14

enum aws_s3_errors {
    AWS_ERROR_S3_MISSING_CONTENT_RANGE_HEADER = AWS_ERROR_ENUM_BEGIN_RANGE(AWS_C_S3_PACKAGE_ID),
    AWS_ERROR_S3_INVALID_CONTENT_RANGE_HEADER,
    AWS_ERROR_S3_MISSING_CONTENT_LENGTH_HEADER,
    AWS_ERROR_S3_INVALID_CONTENT_LENGTH_HEADER,
    AWS_ERROR_S3_MISSING_ETAG,
    AWS_ERROR_S3_INTERNAL_ERROR,
    AWS_ERROR_S3_SLOW_DOWN,
    AWS_ERROR_S3_INVALID_RESPONSE_STATUS,
    AWS_ERROR_S3_MISSING_UPLOAD_ID,
    AWS_ERROR_S3_PROXY_PARSE_FAILED,
    AWS_ERROR_S3_UNSUPPORTED_PROXY_SCHEME,
    AWS_ERROR_S3_CANCELED,
    AWS_ERROR_S3_INVALID_RANGE_HEADER,
    AWS_ERROR_S3_MULTIRANGE_HEADER_UNSUPPORTED,
    AWS_ERROR_S3_RESPONSE_CHECKSUM_MISMATCH,
    AWS_ERROR_S3_CHECKSUM_CALCULATION_FAILED,
    AWS_ERROR_S3_PAUSED,
    AWS_ERROR_S3_LIST_PARTS_PARSE_FAILED,
    AWS_ERROR_S3_RESUMED_PART_CHECKSUM_MISMATCH,
    AWS_ERROR_S3_RESUME_FAILED,
    AWS_ERROR_S3_OBJECT_MODIFIED,
    AWS_ERROR_S3_NON_RECOVERABLE_ASYNC_ERROR,
    AWS_ERROR_S3_METRIC_DATA_NOT_AVAILABLE,
    AWS_ERROR_S3_INCORRECT_CONTENT_LENGTH,
    AWS_ERROR_S3_REQUEST_TIME_TOO_SKEWED,
    AWS_ERROR_S3_FILE_MODIFIED,
    AWS_ERROR_S3_EXCEEDS_MEMORY_LIMIT,
    AWS_ERROR_S3_INVALID_MEMORY_LIMIT_CONFIG,
    AWS_ERROR_S3EXPRESS_CREATE_SESSION_FAILED,
    AWS_ERROR_S3_INTERNAL_PART_SIZE_MISMATCH_RETRYING_WITH_RANGE,
    AWS_ERROR_S3_REQUEST_HAS_COMPLETED,
    AWS_ERROR_S3_RECV_FILE_ALREADY_EXISTS,
    AWS_ERROR_S3_RECV_FILE_NOT_FOUND,
    AWS_ERROR_S3_REQUEST_TIMEOUT,

    AWS_ERROR_S3_END_RANGE = AWS_ERROR_ENUM_END_RANGE(AWS_C_S3_PACKAGE_ID)
};

enum aws_s3_subject {
    AWS_LS_S3_GENERAL = AWS_LOG_SUBJECT_BEGIN_RANGE(AWS_C_S3_PACKAGE_ID),
    AWS_LS_S3_CLIENT,
    AWS_LS_S3_CLIENT_STATS,
    AWS_LS_S3_REQUEST,
    AWS_LS_S3_META_REQUEST,
    AWS_LS_S3_ENDPOINT,
    AWS_LS_S3_LAST = AWS_LOG_SUBJECT_END_RANGE(AWS_C_S3_PACKAGE_ID)
};

struct aws_s3_platform_info;

struct aws_s3_cpu_group_info {
    /* group index, this usually refers to a particular numa node */
    uint16_t cpu_group;
    /* array of network devices on this node */
    struct aws_byte_cursor *nic_name_array;
    /* length of network devices array */
    size_t nic_name_array_length;
    size_t cpus_in_group;
};

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4626) /* assignment operator was implicitly defined as deleted */
#    pragma warning(disable : 5027) /* move assignment operator was implicitly defined as deleted */
#endif

struct aws_s3_platform_info {
    /* name of the instance-type: example c5n.18xlarge */
    struct aws_byte_cursor instance_type;
    /* max throughput for this instance type, in gigabits per second */
    double max_throughput_gbps;
    /* array of cpu group info. This will always have at least one entry. */
    struct aws_s3_cpu_group_info *cpu_group_info_array;
    /* length of cpu group info array */
    size_t cpu_group_info_array_length;

    /* The current build of this library specifically knows an optimal configuration for this
     * platform */
    bool has_recommended_configuration;
};

#ifdef _MSC_VER
#    pragma warning(pop)
#endif

AWS_EXTERN_C_BEGIN

/**
 * Initializes internal datastructures used by aws-c-s3.
 * Must be called before using any functionality in aws-c-s3.
 */
AWS_S3_API
void aws_s3_library_init(struct aws_allocator *allocator);

/**
 * Shuts down the internal datastructures used by aws-c-s3.
 */
AWS_S3_API
void aws_s3_library_clean_up(void);

/*
 * Returns the aws_s3_platform_info for current platform
 * NOTE: THIS API IS EXPERIMENTAL AND UNSTABLE
 */
AWS_S3_API
const struct aws_s3_platform_info *aws_s3_get_current_platform_info(void);

/*
 * Returns the ec2 instance_type for current platform if possible
 * NOTE: THIS API IS EXPERIMENTAL AND UNSTABLE
 */
AWS_S3_API
struct aws_byte_cursor aws_s3_get_current_platform_ec2_intance_type(bool cached_only);

/*
 * Retrieves a list of EC2 instance types with recommended configuration.
 * Returns aws_array_list<aws_byte_cursor>. The caller is responsible for cleaning up the array list.
 */
AWS_S3_API
struct aws_array_list aws_s3_get_platforms_with_recommended_config(void);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_S3_H */
