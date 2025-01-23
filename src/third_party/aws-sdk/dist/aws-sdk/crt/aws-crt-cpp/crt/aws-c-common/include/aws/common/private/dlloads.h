#ifndef AWS_COMMON_PRIVATE_DLLOADS_H
#define AWS_COMMON_PRIVATE_DLLOADS_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * definition is here: https://linux.die.net/man/2/set_mempolicy
 */
#define AWS_MPOL_PREFERRED_ALIAS 1

struct bitmask;

extern long (*g_set_mempolicy_ptr)(int, const unsigned long *, unsigned long);
extern int (*g_numa_available_ptr)(void);
extern int (*g_numa_num_configured_nodes_ptr)(void);
extern int (*g_numa_num_possible_cpus_ptr)(void);
extern int (*g_numa_node_of_cpu_ptr)(int cpu);
extern void *g_libnuma_handle;

#endif /* AWS_COMMON_PRIVATE_DLLOADS_H */
