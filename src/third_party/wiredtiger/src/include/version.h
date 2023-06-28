/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_VERSION --
 *	Structure to represent version information.
 */
struct __wt_version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
};

/*
 * WT_BTREE_VERSION is an alias of WT_VERSION to avoid confusion between btree version numbers and
 * WiredTiger version numbers.
 */
typedef WT_VERSION WT_BTREE_VERSION;

#define WT_NO_VALUE UINT16_MAX
/*
 * Default version to use when none is defined.
 */
#define WT_NO_VERSION ((WT_VERSION){WT_NO_VALUE, WT_NO_VALUE, WT_NO_VALUE})

/*
 * __wt_version_cmp --
 *     Compare two versions to determine whether the first version is greater than, equal to, or
 *     less than the second. As in strcmp() return 1 for greater, 0 for equal, and -1 for less than.
 */
static inline int32_t
__wt_version_cmp(WT_VERSION v, WT_VERSION other)
{
    /*
     * The patch version is not always set for both inputs. In these cases we ignore comparison of
     * patch version by setting them both to the same value. The inputs are pass-by-value and will
     * not be modified by this.
     */
    if (v.patch == WT_NO_VALUE || other.patch == WT_NO_VALUE)
        v.patch = other.patch = 0;

    if (v.major == other.major && v.minor == other.minor && v.patch == other.patch)
        return (0);

    if (v.major > other.major)
        return (1);
    if (v.major == other.major && v.minor > other.minor)
        return (1);
    if (v.major == other.major && v.minor == other.minor && v.patch > other.patch)
        return (1);

    return (-1);
}

/*
 * __wt_version_defined --
 *     Return true if the version has been properly defined with non-default values. Valid versions
 *     do not require the patch version to be set.
 */
static inline bool
__wt_version_defined(WT_VERSION v)
{
    return (v.major != WT_NO_VALUE && v.minor != WT_NO_VALUE);
}

/*
 * __wt_version_eq --
 *     Return true if two versions are equal.
 */
static inline bool
__wt_version_eq(WT_VERSION v, WT_VERSION other)
{
    return (__wt_version_cmp(v, other) == 0);
}

/*
 * __wt_version_gt --
 *     Return true if a version is greater than another version.
 */
static inline bool
__wt_version_gt(WT_VERSION v, WT_VERSION other)
{
    return (__wt_version_cmp(v, other) == 1);
}

/*
 * __wt_version_gte --
 *     Return true if a version is greater than or equal to another version.
 */
static inline bool
__wt_version_gte(WT_VERSION v, WT_VERSION other)
{
    return (__wt_version_cmp(v, other) != -1);
}

/*
 * __wt_version_lt --
 *     Return true if a version is less than another version.
 */
static inline bool
__wt_version_lt(WT_VERSION v, WT_VERSION other)
{
    return (__wt_version_cmp(v, other) == -1);
}

/*
 * __wt_version_lte --
 *     Return true if a version is less than or equal to another version.
 */
static inline bool
__wt_version_lte(WT_VERSION v, WT_VERSION other)
{
    return (__wt_version_cmp(v, other) != 1);
}
