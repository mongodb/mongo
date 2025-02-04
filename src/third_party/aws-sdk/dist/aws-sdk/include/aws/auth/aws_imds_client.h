#ifndef AWS_AUTH_IMDS_CLIENT_H
#define AWS_AUTH_IMDS_CLIENT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/auth/auth.h>
#include <aws/auth/credentials.h>
#include <aws/common/array_list.h>
#include <aws/common/date_time.h>
#include <aws/http/connection_manager.h>
#include <aws/io/retry_strategy.h>

AWS_PUSH_SANE_WARNING_LEVEL

typedef void(aws_imds_client_shutdown_completed_fn)(void *user_data);

/**
 * Optional callback and user data to be invoked when an imds client has fully shut down
 */
struct aws_imds_client_shutdown_options {
    aws_imds_client_shutdown_completed_fn *shutdown_callback;
    void *shutdown_user_data;
};

/**
 * Configuration options when creating an imds client
 */
struct aws_imds_client_options {
    /*
     * Completion callback to be invoked when the client has fully shut down
     */
    struct aws_imds_client_shutdown_options shutdown_options;

    /*
     * Client bootstrap to use when this client makes network connections
     */
    struct aws_client_bootstrap *bootstrap;

    /*
     * Retry strategy instance that governs how failed requests are retried
     */
    struct aws_retry_strategy *retry_strategy;

    /*
     * What version of the imds protocol to use
     *
     * Defaults to IMDS_PROTOCOL_V2
     */
    enum aws_imds_protocol_version imds_version;

    /*
     * If true, fallback from v2 to v1 will be disabled for all cases
     */
    bool ec2_metadata_v1_disabled;

    /*
     * Table holding all cross-system functional dependencies for an imds client.
     *
     * For mocking the http layer in tests, leave NULL otherwise
     */
    struct aws_auth_http_system_vtable *function_table;
};

/*
 * Standard callback for instance metadata queries
 */
typedef void(
    aws_imds_client_on_get_resource_callback_fn)(const struct aws_byte_buf *resource, int error_code, void *user_data);

/**
 * https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instancedata-data-categories.html
 */
struct aws_imds_iam_profile {
    struct aws_date_time last_updated;
    struct aws_byte_cursor instance_profile_arn;
    struct aws_byte_cursor instance_profile_id;
};

/**
 * Block of per-instance EC2-specific data
 *
 * https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instance-identity-documents.html
 */
struct aws_imds_instance_info {
    /* an array of aws_byte_cursor */
    struct aws_array_list marketplace_product_codes;
    struct aws_byte_cursor availability_zone;
    struct aws_byte_cursor private_ip;
    struct aws_byte_cursor version;
    struct aws_byte_cursor instance_id;
    /* an array of aws_byte_cursor */
    struct aws_array_list billing_products;
    struct aws_byte_cursor instance_type;
    struct aws_byte_cursor account_id;
    struct aws_byte_cursor image_id;
    struct aws_date_time pending_time;
    struct aws_byte_cursor architecture;
    struct aws_byte_cursor kernel_id;
    struct aws_byte_cursor ramdisk_id;
    struct aws_byte_cursor region;
};

/* the item typed stored in array is pointer to aws_byte_cursor */
typedef void(
    aws_imds_client_on_get_array_callback_fn)(const struct aws_array_list *array, int error_code, void *user_data);

typedef void(aws_imds_client_on_get_credentials_callback_fn)(
    const struct aws_credentials *credentials,
    int error_code,
    void *user_data);

typedef void(aws_imds_client_on_get_iam_profile_callback_fn)(
    const struct aws_imds_iam_profile *iam_profile_info,
    int error_code,
    void *user_data);

typedef void(aws_imds_client_on_get_instance_info_callback_fn)(
    const struct aws_imds_instance_info *instance_info,
    int error_code,
    void *user_data);

/**
 * AWS EC2 Metadata Client is used to retrieve AWS EC2 Instance Metadata info.
 */
struct aws_imds_client;

AWS_EXTERN_C_BEGIN

/**
 * Creates a new imds client
 *
 * @param allocator memory allocator to use for creation and queries
 * @param options configuration options for the imds client
 *
 * @return a newly-constructed imds client, or NULL on failure
 */
AWS_AUTH_API
struct aws_imds_client *aws_imds_client_new(
    struct aws_allocator *allocator,
    const struct aws_imds_client_options *options);

/**
 * Increments the ref count on the client
 *
 * @param client imds client to acquire a reference to
 */
AWS_AUTH_API
void aws_imds_client_acquire(struct aws_imds_client *client);

/**
 * Decrements the ref count on the client
 *
 * @param client imds client to release a reference to
 */
AWS_AUTH_API
void aws_imds_client_release(struct aws_imds_client *client);

/**
 * Queries a generic resource (string) from the ec2 instance metadata document
 *
 * @param client imds client to use for the query
 * @param resource_path path of the resource to query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_resource_async(
    struct aws_imds_client *client,
    struct aws_byte_cursor resource_path,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the ami id of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_ami_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the ami launch index of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_ami_launch_index(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the ami manifest path of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_ami_manifest_path(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the list of ancestor ami ids of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_ancestor_ami_ids(
    struct aws_imds_client *client,
    aws_imds_client_on_get_array_callback_fn callback,
    void *user_data);

/**
 * Gets the instance-action of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_instance_action(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the instance id of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_instance_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the instance type of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_instance_type(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the mac address of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_mac_address(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the private ip address of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_private_ip_address(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the availability zone of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_availability_zone(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the product codes of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_product_codes(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the public key of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_public_key(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the ramdisk id of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_ramdisk_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the reservation id of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_reservation_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the list of the security groups of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_security_groups(
    struct aws_imds_client *client,
    aws_imds_client_on_get_array_callback_fn callback,
    void *user_data);

/**
 * Gets the list of block device mappings of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_block_device_mapping(
    struct aws_imds_client *client,
    aws_imds_client_on_get_array_callback_fn callback,
    void *user_data);

/**
 * Gets the attached iam role of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_attached_iam_role(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets temporary credentials based on the attached iam role of the ec2 instance
 *
 * @param client imds client to use for the query
 * @param iam_role_name iam role name to get temporary credentials through
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_credentials(
    struct aws_imds_client *client,
    struct aws_byte_cursor iam_role_name,
    aws_imds_client_on_get_credentials_callback_fn callback,
    void *user_data);

/**
 * Gets the iam profile information of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_iam_profile(
    struct aws_imds_client *client,
    aws_imds_client_on_get_iam_profile_callback_fn callback,
    void *user_data);

/**
 * Gets the user data of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_user_data(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the signature of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_instance_signature(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data);

/**
 * Gets the instance information data block of the ec2 instance from the instance metadata document
 *
 * @param client imds client to use for the query
 * @param callback callback function to invoke on query success or failure
 * @param user_data opaque data to invoke the completion callback with
 * @return AWS_OP_SUCCESS if the query was successfully started, AWS_OP_ERR otherwise
 */
AWS_AUTH_API
int aws_imds_client_get_instance_info(
    struct aws_imds_client *client,
    aws_imds_client_on_get_instance_info_callback_fn callback,
    void *user_data);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_AUTH_IMDS_CLIENT_H */
