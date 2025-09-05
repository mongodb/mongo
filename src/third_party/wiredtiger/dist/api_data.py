#!/usr/bin/env python3

# This file is a python script that describes the WiredTiger API.

class Method:
    def __init__(self, config, compilable=False):
        # Deal with duplicates: with complex configurations (like WT_SESSION::create), it's simpler
        # to deal with duplicates once than manually as configurations are defined.
        self.config = []
        self.compilable = compilable
        lastname = None
        for c in sorted(config):
            if '.' in c.name:
                raise "Bad config key '%s'" % c.name
            if c.name == lastname:
                continue
            lastname = c.name
            self.config.append(c)

class Config:
    def __init__(self, name, default, desc, subconfig=None, **flags):
        self.name = name
        self.default = default
        self.desc = desc
        self.subconfig = subconfig
        self.flags = flags

    # Comparators for sorting.
    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __lt__(self, other):
        return self.name < other.name

    def __le__(self, other):
        return self.name <= other.name

    def __gt__(self, other):
        return self.name > other.name

    def __ge__(self, other):
        return self.name >= other.name

common_runtime_config = [
    Config('app_metadata', '', r'''
        application-owned metadata for this object'''),
    Config('assert', '', r'''
        declare timestamp usage''',
        type='category', subconfig= [
        Config('commit_timestamp', 'none', r'''
            this option is no longer supported, retained for backward compatibility''',
            choices=['always', 'key_consistent', 'never', 'none'], undoc=True),
        Config('durable_timestamp', 'none', r'''
            this option is no longer supported, retained for backward compatibility''',
            choices=['always', 'key_consistent', 'never', 'none'], undoc=True),
        Config('read_timestamp', 'none', r'''
            if set, check that timestamps are \c always or \c never used on reads with this table,
            writing an error message if the policy is violated. If the library was built in
            diagnostic mode, drop core at the failing check''',
            choices=['always', 'never', 'none']),
        Config('write_timestamp', 'off', r'''
            if set, check that timestamps are used consistently with the configured
            \c write_timestamp_usage option for this table, writing an error message if the policy
            is violated. If the library was built in diagnostic mode, drop core at the failing
            check''',
            choices=['off', 'on'], undoc=True),
        ]),
    Config('verbose', '[]', r'''
        this option is no longer supported, retained for backward compatibility''',
        type='list', choices=['write_timestamp'], undoc=True),
    Config('write_timestamp_usage', 'none', r'''
        describe how timestamps are expected to be used on table modifications. The choices
        are the default, which ensures that once timestamps are used for a key, they are always
        used, and also that multiple updates to a key never use decreasing timestamps and
        \c never which enforces that timestamps are never used for a table. (The \c always,
        \c key_consistent, \c mixed_mode and \c ordered choices should not be used, and are
        retained for backward compatibility.)''',
        choices=['always', 'key_consistent', 'mixed_mode', 'never', 'none', 'ordered']),
]

# Metadata shared by all schema objects
common_meta = common_runtime_config + [
    Config('collator', 'none', r'''
        configure custom collation for keys. Permitted values are \c "none" or a custom collator
        name created with WT_CONNECTION::add_collator'''),
    Config('columns', '', r'''
        list of the column names. Comma-separated list of the form <code>(column[,...])</code>.
        For tables, the number of entries must match the total number of values in \c key_format
        and \c value_format. For colgroups and indices, all column names must appear in the
        list of columns for the table''',
        type='list'),
]

source_meta = [
    Config('source', '', r'''
        set a custom data source URI for a column group, index or simple table. By default,
        the data source URI is derived from the \c type and the column group or index name.
        Applications can create tables from existing data sources by supplying a \c source
        configuration''',
        undoc=True),
    Config('type', 'file', r'''
        set the type of data source used to store a column group, index or simple table.
        By default, a \c "file:" URI is derived from the object name. The \c type configuration
        can be used to switch to a different data source or an extension configured by the
        application'''),
]

connection_page_delta_config_common = [
    Config('delta_pct', '20', r'''
        the size threshold (as a percentage) at which a delta will cease to be emitted
        when reconciling a page. For example, if this is set to 20, the size of a delta
        is 20 bytes, and the size of the full page image is 100 bytes, reconciliation
        can emit a delta for the page (if various other preconditions are met).
        Conversely, if the delta came to 21 bytes, reconciliation would not emit a
        delta. Deltas larger than full pages are permitted for measurement and testing
        reasons, and may be disallowed in future.''', min='1', max='1000', type='int', undoc=True),
    Config('flatten_leaf_page_delta', 'false', r'''
        When enabled, page read rewrites the leaf pages with deltas to a new
        disk image if successful''',
        type='boolean', undoc=True),
    Config('internal_page_delta', 'true', r'''
        When enabled, reconciliation may write deltas for internal pages
        instead of writing entire pages every time''',
        type='boolean', undoc=True),
    Config('leaf_page_delta', 'true', r'''
        When enabled, reconciliation may write deltas for leaf pages
        instead of writing entire pages every time''',
        type='boolean', undoc=True),
    Config('max_consecutive_delta', '32', r'''
        the max consecutive deltas allowed for a single page. The maximum value is set
        at 32 (WT_DELTA_LIMIT). If we need to change that, please change WT_DELTA_LIMIT
        as well.''', min='1', max='32', type='int', undoc=True),
]
connection_disaggregated_config_common = [
    Config('checkpoint_meta', '', r'''
        the checkpoint metadata from which to start (or restart) the node''',
        undoc=True),
    Config('last_materialized_lsn', '', r'''
        the page LSN indicating that all pages up until this LSN are available for reading''',
        type='int', undoc=True),
    Config('local_files_action', 'delete', r'''
        what should be done to the local files in disaggregated mode upon startup.''',
        choices=['delete', 'fail', 'ignore'], undoc=True),
    Config('lose_all_my_data', 'false', r'''
        This setting skips file system syncs, and will cause data loss outside of a
        disaggregated storage context.''',
        type='boolean', undoc=True),
    Config('role', '', r'''
        whether the stable table in a layered data store should lead or follow''',
        choices=['leader', 'follower'], undoc=True),
]
disaggregated_config_common = [
    Config('page_log', '', r'''
        The page log service used as a backing for this table. This is used experimentally
        by layered tables to back their stable component in shared/object based storage''',
        type='string', undoc=True),
]
connection_disaggregated_config = [
    Config('disaggregated', '', r'''
        configure disaggregated storage for this connection''',
        type='category', subconfig=connection_disaggregated_config_common +\
              disaggregated_config_common),
]
connection_page_delta_config = [
    Config('page_delta', '', r'''
        configure page delta settings for this connection''',
        type='category', subconfig=connection_page_delta_config_common),
]
file_disaggregated_config = [
    Config('disaggregated', '', r'''
        configure disaggregated storage for this file''',
        type='category', subconfig=disaggregated_config_common
    ),
]
wiredtiger_open_disaggregated_storage_configuration = connection_disaggregated_config
connection_reconfigure_disaggregated_configuration = [
    Config('disaggregated', '', r'''
        configure disaggregated storage for this connection''',
        type='category', subconfig=connection_disaggregated_config_common),
]
wiredtiger_open_page_delta_configuration = connection_page_delta_config
connection_reconfigure_page_delta_configuration = [
    Config('page_delta', '', r'''
        configure page delta settings for this connection''',
        type='category', subconfig=connection_page_delta_config_common),
]

format_meta = common_meta + [
    Config('key_format', 'u', r'''
        the format of the data packed into key items. See @ref schema_format_types for details.
        By default, the key_format is \c 'u' and applications use WT_ITEM structures to manipulate
        raw byte arrays. By default, records are stored in row-store files: keys of type \c 'r'
        are record numbers and records referenced by record number are stored in column-store
        files''',
        type='format', func='__wt_struct_confchk'),
    Config('value_format', 'u', r'''
        the format of the data packed into value items. See @ref schema_format_types for details.
        By default, the value_format is \c 'u' and applications use a WT_ITEM structure to
        manipulate raw byte arrays. Value items of type 't' are bitfields, and when configured
        with record number type keys, will be stored using a fixed-length store''',
        type='format', func='__wt_struct_confchk'),
]

# We need to be able to understand these forever, even if they're just empty strings. It's OK
# to reject them if there's any real config -- the feature is gone.
lsm_config = [
    Config('lsm', '', r'''
        Removed options, preserved to allow parsing old metadata''',
        type='category', undoc=True, subconfig=[
        Config('auto_throttle', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('bloom', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('bloom_bit_count', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('bloom_config', '', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('bloom_hash_count', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('bloom_oldest', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('chunk_count_limit', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('chunk_max', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('chunk_size', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('merge_max', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
        Config('merge_min', 'none', r'''
            removed option, preserved to allow parsing old metadata''', undoc=True),
    ]),
]

tiered_config = [
    Config('tiered_storage', '', r'''
        configure a storage source for this table''',
        type='category', subconfig=[
        Config('name', 'none', r'''
            permitted values are \c "none" or a custom storage source name created with
            WT_CONNECTION::add_storage_source. See @ref custom_storage_sources for more
            information'''),
        Config('auth_token', '', r'''
            authentication string identifier'''),
        Config('bucket', '', r'''
            the bucket indicating the location for this table'''),
        Config('bucket_prefix', '', r'''
            the unique bucket prefix for this table'''),
        Config('cache_directory', '', r'''
            a directory to store locally cached versions of files in the storage source. By
            default, it is named with \c "-cache" appended to the bucket name. A relative
            directory name is relative to the home directory'''),
        Config('local_retention', '300', r'''
            time in seconds to retain data on tiered storage on the local tier for faster
            read access''',
            min='0', max='10000'),
        Config('object_target_size', '0', r'''
            this option is no longer supported, retained for backward compatibility''',
            min='0', undoc=True),
        Config('shared', 'false', r'''
            enable sharing tiered tables across other WiredTiger instances.''',
            type='boolean'),
        ]),
]

tiered_tree_config = [
    Config('bucket', '', r'''
        the bucket indicating the location for this table'''),
    Config('bucket_prefix', '', r'''
        the unique bucket prefix for this table'''),
    Config('cache_directory', '', r'''
        a directory to store locally cached versions of files in the storage source. By default,
        it is named with \c "-cache" appended to the bucket name. A relative directory name
        is relative to the home directory'''),
]

log_runtime_config = [
    Config('log', '', r'''
        the transaction log configuration for this object. Only valid if \c log is enabled in
        ::wiredtiger_open''',
        type='category', subconfig=[
        Config('enabled', 'true', r'''
            if false, this object has checkpoint-level durability''',
            type='boolean'),
        ]),
]

file_runtime_config = common_runtime_config + log_runtime_config + [
    Config('access_pattern_hint', 'none', r'''
        It is recommended that workloads that consist primarily of updates and/or point queries
        specify \c random. Workloads that do many cursor scans through large ranges of data
        should specify \c sequential and other workloads should specify \c none. The option leads
        to an appropriate operating system advisory call where available''',
        choices=['none', 'random', 'sequential']),
    Config('cache_resident', 'false', r'''
        do not ever evict the object's pages from cache, see @ref tuning_cache_resident for more
        information''',
        type='boolean'),
    Config('os_cache_max', '0', r'''
        maximum system buffer cache usage, in bytes. If non-zero, evict object blocks from
        the system buffer cache after that many bytes from this object are read or written into
        the buffer cache''',
        min=0),
    Config('os_cache_dirty_max', '0', r'''
        maximum dirty system buffer cache usage, in bytes. If non-zero, schedule writes for
        dirty blocks belonging to this object in the system buffer cache after that many bytes
        from this object are written into the buffer cache''',
        min=0),
]

# Per-file configuration
file_config = format_meta + file_runtime_config + tiered_config + file_disaggregated_config + [
    Config('block_allocation', 'best', r'''
        configure block allocation. Permitted values are \c "best" or \c "first"; the \c "best"
        configuration uses a best-fit algorithm, the \c "first" configuration uses a
        first-available algorithm during block allocation''',
        choices=['best', 'first',]),
    Config('allocation_size', '4KB', r'''
        the file unit allocation size, in bytes, must be a power of two; smaller values decrease
        the file space required by overflow items, and the default value of 4KB is a good choice
        absent requirements from the operating system or storage device''',
        min='512B', max='128MB'),
    Config('block_compressor', 'none', r'''
        configure a compressor for file blocks. Permitted values are \c "none" or a custom
        compression engine name created with WT_CONNECTION::add_compressor. If WiredTiger
        has builtin support for \c "lz4", \c "snappy", \c "zlib" or \c "zstd" compression,
        these names are also available. See @ref compression for more information'''),
    Config('block_manager', 'default', r'''
        configure a manager for file blocks. Permitted values are \c "default" or the
        disaggregated storage block manager backed by \c PALI.''',
        choices=['default', 'disagg']),
    Config('checksum', 'on', r'''
        configure block checksums; the permitted values are \c on, \c off, \c uncompressed and
        \c unencrypted. The default is \c on, in which case all block writes include a checksum
        subsequently verified when the block is read. The \c off setting does no checksums,
        the \c uncompressed setting only checksums blocks that are not compressed, and the
        \c unencrypted setting only checksums blocks that are not encrypted. See @ref
        tune_checksum for more information.''',
        choices=['on', 'off', 'uncompressed', 'unencrypted']),
    Config('dictionary', '0', r'''
        the maximum number of unique values remembered in the row-store/variable-length column-store
        leaf page value dictionary; see @ref file_formats_compression for more information''',
        min='0'),
    Config('encryption', '', r'''
        configure an encryptor for file blocks. When a table is created, its encryptor is not
        implicitly used for any related indices or column groups''',
        type='category', subconfig=[
        Config('name', 'none', r'''
            Permitted values are \c "none" or a custom encryption engine name created with
            WT_CONNECTION::add_encryptor. See @ref encryption for more information'''),
        Config('keyid', '', r'''
            An identifier that identifies a unique instance of the encryptor. It is stored in
            clear text, and thus is available when the WiredTiger database is reopened. On the
            first use of a (name, keyid) combination, the WT_ENCRYPTOR::customize function is
            called with the keyid as an argument'''),
        ]),
    Config('format', 'btree', r'''
        the file format''',
        choices=['btree']),
    Config('huffman_key', 'none', r'''
        This option is no longer supported, retained for backward compatibility''', undoc=True),
    Config('huffman_value', 'none', r'''
        This option is no longer supported, retained for backward compatibility''', undoc=True),
    Config('ignore_in_memory_cache_size', 'false', r'''
        allow update and insert operations to proceed even if the cache is already at
        capacity. Only valid in conjunction with in-memory databases. Should be used with caution -
        this configuration allows WiredTiger to consume memory over the configured cache limit''',
        type='boolean'),
    Config('internal_key_truncate', 'true', r'''
        configure internal key truncation, discarding unnecessary trailing bytes on internal keys
        (ignored for custom collators)''',
        type='boolean'),
    Config('internal_page_max', '4KB', r'''
        the maximum page size for internal nodes, in bytes; the size must be a multiple of the
        allocation size and is significant for applications wanting to avoid excessive L2 cache
        misses while searching the tree. The page maximum is the bytes of uncompressed data,
        that is, the limit is applied before any block compression is done''',
        min='512B', max='512MB'),
    Config('internal_item_max', '0', r'''
        This option is no longer supported, retained for backward compatibility''',
        min=0, undoc=True),
    Config('internal_key_max', '0', r'''
        This option is no longer supported, retained for backward compatibility''',
        min='0'),
    Config('in_memory', 'false', r'''
        keep the tree data in memory. Used experimentally by layered tables''',
        type='boolean', undoc=True),
    Config('key_gap', '10', r'''
        This option is no longer supported, retained for backward compatibility''',
        min='0'),
    Config('leaf_key_max', '0', r'''
        the largest key stored in a leaf node, in bytes. If set, keys larger than the specified
        size are stored as overflow items (which may require additional I/O to access).
        The default value is one-tenth the size of a newly split leaf page''',
        min='0'),
    Config('leaf_page_max', '32KB', r'''
        the maximum page size for leaf nodes, in bytes; the size must be a multiple of the
        allocation size, and is significant for applications wanting to maximize sequential data
        transfer from a storage device. The page maximum is the bytes of uncompressed data,
        that is, the limit is applied before any block compression is done. For fixed-length
        column store, the size includes only the bitmap data; pages containing timestamp
        information can be larger, and the size is limited to 128KB rather than 512MB''',
        min='512B', max='512MB'),
    Config('leaf_value_max', '0', r'''
        the largest value stored in a leaf node, in bytes. If set, values larger than the
        specified size are stored as overflow items (which may require additional I/O to
        access). If the size is larger than the maximum leaf page size, the page size is
        temporarily ignored when large values are written. The default is one-half the size of
        a newly split leaf page''',
        min='0'),
    Config('leaf_item_max', '0', r'''
        This option is no longer supported, retained for backward compatibility''',
        min=0, undoc=True),
    Config('memory_page_image_max', '0', r'''
        the maximum in-memory page image represented by a single storage block. Depending on
        compression efficiency, compression can create storage blocks which require significant
        resources to re-instantiate in the cache, penalizing the performance of future point
        updates. The value limits the maximum in-memory page image a storage block will need. If
        set to 0, a default of 4 times \c leaf_page_max is used''',
        min='0'),
    Config('memory_page_max', '5MB', r'''
        the maximum size a page can grow to in memory before being reconciled to disk. The
        specified size will be adjusted to a lower bound of <code>leaf_page_max</code>, and an
        upper bound of <code>cache_size / 10</code>. This limit is soft - it is possible for
        pages to be temporarily larger than this value.''',
        min='512B', max='10TB'),
    Config('prefix_compression', 'false', r'''
        configure prefix compression on row-store leaf pages''',
        type='boolean'),
    Config('prefix_compression_min', '4', r'''
        minimum gain before prefix compression will be used on row-store leaf pages''',
        min=0),
    Config('split_deepen_min_child', '0', r'''
        minimum entries in a page to consider deepening the tree. Pages will be considered for
        splitting and deepening the search tree as soon as there are more than the configured
        number of children''',
        type='int', undoc=True),
    Config('split_deepen_per_child', '0', r'''
        entries allocated per child when deepening the tree''',
        type='int', undoc=True),
    Config('split_pct', '90', r'''
        the Btree page split size as a percentage of the maximum Btree page size, that is,
        when a Btree page is split, it will be split into smaller pages, where each page is
        the specified percentage of the maximum Btree page size''',
        min='50', max='100'),
]

# File metadata, including both configurable and non-configurable (internal)
file_meta = file_config + [
    Config('checkpoint', '', r'''
        the file checkpoint entries'''),
    Config('checkpoint_backup_info', '', r'''
        the incremental backup durable information'''),
    Config('checkpoint_lsn', '', r'''
        LSN of the last checkpoint'''),
    Config('id', '', r'''
        the file's ID number'''),
    Config('live_restore', '', r'''
        live restore metadata for a file''', type='category', subconfig=[
            Config('bitmap', '', r'''bitmap representation of a file'''),
            Config('nbits', 0, r'''the number of bits in the bitmap as an integer''', type='int')]),
    Config('readonly', 'false', r'''
        the file is read-only. All methods that modify a file are disabled. See @ref
        readonly for more information''',
        type='boolean'),
    Config('tiered_object', 'false', r'''
        this file is a tiered object. When opened on its own, it is marked as readonly and may
        be restricted in other ways''',
        type='boolean', undoc=True),
    Config('version', '(major=0,minor=0)', r'''
        the file version'''),
]

tiered_meta = file_meta + tiered_config + [
    Config('flush_time', '0', r'''
        indicates the time this tree was flushed to shared storage or 0 if unflushed'''),
    Config('flush_timestamp', '0', r'''
        timestamp at which this tree was flushed to shared storage or 0 if unflushed'''),
    Config('last', '0', r'''
        the last allocated object ID'''),
    Config('oldest', '1', r'''
        the oldest allocated object ID'''),
    Config('tiers', '', r'''
        list of data sources to combine into a tiered storage structure''',
        type='list'),
]

tier_meta = file_meta + tiered_tree_config

# Objects need to have the readonly setting set and bucket_prefix.
# The file_meta already contains those pieces.
object_meta = file_meta + [
    Config('flush_time', '0', r'''
        indicates the time this object was flushed to shared storage or 0 if unflushed'''),
    Config('flush_timestamp', '0', r'''
        timestamp at which this object was flushed to shared storage or 0 if unflushed'''),
]

table_only_config = [
    Config('colgroups', '', r'''
        comma-separated list of names of column groups. Each column group is stored separately,
        keyed by the primary key of the table. If no column groups are specified, all columns
        are stored together in a single file. All value columns in the table must appear in
        at least one column group. Each column group must be created with a separate call to
        WT_SESSION::create using a \c colgroup: URI''',
        type='list'),
]

index_only_config = [
    Config('extractor', 'none', r'''
        removed option, preserved to allow parsing old metadata''', undoc=True),
    Config('immutable', 'false', r'''
        configure the index to be immutable -- that is, the index is not changed by any update to
        a record in the table''',
        type='boolean'),
]

colgroup_meta = common_meta + source_meta

index_meta = format_meta + source_meta + index_only_config

table_meta = format_meta + table_only_config

layered_config = [
    Config('ingest', '', r'''
        URI for layered ingest table''',
        type='string', undoc=True),
    Config('stable', '', r'''
        URI for layered stable table''',
        type='string', undoc=True),
]

layered_meta = format_meta + layered_config + log_runtime_config + connection_disaggregated_config

# Connection runtime config, shared by conn.reconfigure and wiredtiger_open
connection_runtime_config = [
    Config('block_cache', '', r'''
        block cache configuration options''',
        type='category', subconfig=[
        Config('cache_on_checkpoint', 'true', r'''
            cache blocks written by a checkpoint''',
            type='boolean'),
        Config('cache_on_writes', 'true', r'''
            cache blocks as they are written (other than checkpoint blocks)''',
            type='boolean'),
        Config('enabled', 'false', r'''
            enable block cache''',
            type='boolean'),
        Config('blkcache_eviction_aggression', '1800', r'''
            seconds an unused block remains in the cache before it is evicted''',
            min='1', max='7200'),
        Config('full_target', '95', r'''
            the fraction of the block cache that must be full before eviction will remove
            unused blocks''',
            min='30', max='100'),
        Config('size', '0', r'''
            maximum memory to allocate for the block cache''',
            min='0', max='10TB'),
        Config('hashsize', '32768', r'''
            number of buckets in the hashtable that keeps track of blocks''',
            min='512', max='256K'),
        Config('max_percent_overhead', '10', r'''
            maximum tolerated overhead expressed as the number of blocks added and removed as
            percent of blocks looked up; cache population and eviction will be suppressed if
            the overhead exceeds the threshold''',
            min='1', max='500'),
        Config('nvram_path', '', r'''
            the absolute path to the file system mounted on the NVRAM device'''),
        Config('percent_file_in_dram', '50', r'''
            bypass cache for a file if the set percentage of the file fits in system DRAM
            (as specified by block_cache.system_ram)''',
            min='0', max='100'),
        Config('system_ram', '0', r'''
            the bytes of system DRAM available for caching filesystem blocks''',
            min='0', max='1024GB'),
        Config('type', '', r'''
            cache location: DRAM or NVRAM'''),
        ]),
    Config('cache_eviction_controls', '', r'''
        Controls the experimental incremental cache eviction features.''',
        type='category', subconfig=[
            Config('incremental_app_eviction', 'false', r'''
                Only a part of application threads will participate in cache management 
                when a cache threshold reaches its trigger limit.''',
                type='boolean'),
            Config('scrub_evict_under_target_limit', 'true', 
                r'''Change the eviction strategy to scrub eviction when the cache usage is under
                the target limit.''',
                type='boolean'),
        ]),
    Config('cache_size', '100MB', r'''
        maximum heap memory to allocate for the cache. A database should configure either
        \c cache_size or \c shared_cache but not both''',
        min='1MB', max='10TB'),
    Config('cache_max_wait_ms', '0', r'''
        the maximum number of milliseconds an application thread will wait for space to be
        available in cache before giving up. Default or 0 will wait forever. 1 will never wait''',
        min=0),
    Config('cache_stuck_timeout_ms', '300000', r'''
        the number of milliseconds to wait before a stuck cache times out in diagnostic mode.
        Default will wait for 5 minutes, 0 will wait forever''',
        min=0),
    Config('cache_overhead', '8', r'''
        assume the heap allocator overhead is the specified percentage, and adjust the cache
        usage by that amount (for example, if there is 10GB of data in cache, a percentage of
        10 means WiredTiger treats this as 11GB). This value is configurable because different
        heap allocators have different overhead and different workloads will have different
        heap allocation sizes and patterns, therefore applications may need to adjust this
        value based on allocator choice and behavior in measured workloads''',
        min='0', max='30'),
    Config('checkpoint', '', r'''
        periodically checkpoint the database. Enabling the checkpoint server uses a session
        from the configured \c session_max''',
        type='category', subconfig=[
        Config('log_size', '0', r'''
            wait for this amount of log record bytes to be written to the log between each
            checkpoint. If non-zero, this value will use a minimum of the log file size.
            A database can configure both log_size and wait to set an upper bound for checkpoints;
            setting this value above 0 configures periodic checkpoints''',
            min='0', max='2GB'),
        Config('wait', '0', r'''
            seconds to wait between each checkpoint; setting this value above 0 configures
            periodic checkpoints''',
            min='0', max='100000'),
        ]),
    Config('checkpoint_cleanup', '', r'''
        periodically checkpoint cleanup the database.''',
        type='category', subconfig=[
        Config('method', 'none', r'''
            control how aggressively obsolete content is removed by reading the internal pages.
            Default to none, which means no additional work is done to find obsolete content.
            ''', choices=['none', 'reclaim_space']),
        Config('wait', '300', r'''
            seconds to wait between each checkpoint cleanup''',
            min='1', max='100000'),
        Config('file_wait_ms', '0', r'''
            the number of milliseconds to wait between each file by the checkpoint cleanup,
            0 will not wait''',
            min=0),
        ]),
    Config('debug_mode', '', r'''
        control the settings of various extended debugging features''',
        type='category', subconfig=[
        Config('background_compact', 'false', r'''
               if true, background compact aggressively removes compact statistics for a file and
               decreases the max amount of time a file can be skipped for.''',
               type='boolean'),
        Config('corruption_abort', 'true', r'''
            if true and built in diagnostic mode, dump core in the case of data corruption''',
            type='boolean'),
        Config('checkpoint_retention', '0', r'''
            adjust log removal to retain the log records of this number of checkpoints. Zero
            or one means perform normal removal.''',
            min='0', max='1024'),
        Config('configuration', 'false', r'''
               if true, display invalid cache configuration warnings.''',
               type='boolean'),
        Config('cursor_copy', 'false', r'''
            if true, use the system allocator to make a copy of any data returned by a cursor
            operation and return the copy instead. The copy is freed on the next cursor
            operation. This allows memory sanitizers to detect inappropriate references to
            memory owned by cursors.''',
            type='boolean'),
        Config('cursor_reposition', 'false', r'''
            if true, for operations with snapshot isolation the cursor temporarily releases any page
            that requires force eviction, then repositions back to the page for further operations.
            A page release encourages eviction of hot or large pages, which is more likely to
            succeed without a cursor keeping the page pinned.''',
            type='boolean'),
        Config('eviction', 'false', r'''
            if true, modify internal algorithms to change skew to force history store eviction
            to happen more aggressively. This includes but is not limited to not skewing newest,
            not favoring leaf pages, and modifying the eviction score mechanism.''',
            type='boolean'),
        Config('log_retention', '0', r'''
            adjust log removal to retain at least this number of log files.
            (Warning: this option can remove log files required for recovery if no checkpoints
            have yet been done and the number of log files exceeds the configured value. As
            WiredTiger cannot detect the difference between a system that has not yet checkpointed
            and one that will never checkpoint, it might discard log files before any checkpoint is
            done.) Ignored if set to 0''',
            min='0', max='1024'),
        Config('page_history', 'false', r'''
            if true, keep track of per-page usage statistics for all pages and periodically print a
            report. Currently this works only for disaggregated storage.''',
            type='boolean', undoc=True),
        Config('realloc_exact', 'false', r'''
            if true, reallocation of memory will only provide the exact amount requested. This
            will help with spotting memory allocation issues more easily.''',
            type='boolean'),
        Config('realloc_malloc', 'false', r'''
            if true, every realloc call will force a new memory allocation by using malloc.''',
            type='boolean'),
        Config('rollback_error', '0', r'''
            return a WT_ROLLBACK error from a transaction operation about every Nth operation
            to simulate a collision''',
            min='0', max='10M'),
        Config('slow_checkpoint', 'false', r'''
            if true, slow down checkpoint creation by slowing down internal page processing.''',
            type='boolean'),
        Config('stress_skiplist', 'false', r'''
            Configure various internal parameters to encourage race conditions and other issues
            with internal skip lists, e.g. using a more dense representation.''',
            type='boolean'),
        Config('table_logging', 'false', r'''
            if true, write transaction related information to the log for all operations, even
            operations for tables with logging turned off. This additional logging information
            is intended for debugging and is informational only, that is, it is ignored during
            recovery''',
            type='boolean'),
        Config('tiered_flush_error_continue', 'false', r'''
            on a write to tiered storage, continue when an error occurs.''',
            type='boolean'),
        Config('update_restore_evict', 'false', r'''
            if true, control all dirty page evictions through forcing update restore eviction.''',
            type='boolean'),
        Config('eviction_checkpoint_ts_ordering', 'false', r'''
            if true, act as if eviction is being run in parallel to checkpoint. We should return
            EBUSY in eviction if we detect any timestamp ordering issue.''',
            type='boolean'),
        ]),
    Config('error_prefix', '', r'''
        prefix string for error messages'''),
    Config('eviction', '', r'''
        eviction configuration options''',
        type='category', subconfig=[
            Config('threads_max', '8', r'''
                maximum number of threads WiredTiger will start to help evict pages from cache. The
                number of threads started will vary depending on the current eviction load. Each
                eviction worker thread uses a session from the configured session_max''',
                min=1, max=64), # !!! Must match WT_EVICT_MAX_WORKERS
            Config('threads_min', '1', r'''
                minimum number of threads WiredTiger will start to help evict pages from
                cache. The number of threads currently running will vary depending on the
                current eviction load''',
                min=1, max=64),
            Config('evict_sample_inmem', 'true', r'''
                If no in-memory ref is found on the root page, attempt to locate a random
                in-memory page by examining all entries on the root page.''',
                type='boolean'),
            Config('evict_use_softptr', 'false', r'''
                Experimental: Use "soft pointers" instead of hard hazard
                pointers in eviction server to remember its walking position in the tree. This might
                be preferable to set to "true" if there are many collections. It can improve or
                degrade performance depending on the workload.''',
                type='boolean', undoc=True),
            Config('legacy_page_visit_strategy', 'false', r'''
                Use legacy page visit strategy for eviction. Using this option is highly discouraged
                as it will re-introduce the bug described in WT-9121.''',
                type='boolean'),
            ]),
    Config('eviction_checkpoint_target', '1', r'''
        perform eviction at the beginning of checkpoints to bring the dirty content in cache
        to this level. It is a percentage of the cache size if the value is within the range of
        0 to 100 or an absolute size when greater than 100. The value is not allowed to exceed
        the \c cache_size. Ignored if set to zero.''',
        min=0, max='10TB'),
    Config('eviction_dirty_target', '5', r'''
        perform eviction in worker threads when the cache contains at least this much dirty
        content. It is a percentage of the cache size if the value is within the range of 1 to
        100 or an absolute size when greater than 100. The value is not allowed to exceed the
        \c cache_size and has to be lower than its counterpart \c eviction_dirty_trigger''',
        min=1, max='10TB'),
    Config('eviction_dirty_trigger', '20', r'''
        trigger application threads to perform eviction when the cache contains at least this much
        dirty content. It is a percentage of the cache size if the value is within the range of
        1 to 100 or an absolute size when greater than 100. The value is not allowed to exceed
        the \c cache_size and has to be greater than its counterpart \c eviction_dirty_target.
        This setting only alters behavior if it is lower than eviction_trigger''',
        min=1, max='10TB'),
    Config('eviction_target', '80', r'''
        perform eviction in worker threads when the cache contains at least this much content. It
        is a percentage of the cache size if the value is within the range of 10 to 100 or
        an absolute size when greater than 100. The value is not allowed to exceed the \c
        cache_size and has to be lower than its counterpart \c eviction_trigger''',
        min=10, max='10TB'),
    Config('eviction_trigger', '95', r'''
        trigger application threads to perform eviction when the cache contains at least this
        much content. It is a percentage of the cache size if the value is within the range of
        10 to 100 or an absolute size when greater than 100. The value is not allowed to exceed
        the \c cache_size and has to be greater than its counterpart \c eviction_target''',
        min=10, max='10TB'),
    Config('eviction_updates_target', '0', r'''
        perform eviction in worker threads when the cache contains at least this many bytes of
        updates. It is a percentage of the cache size if the value is within the range of 0 to 100
        or an absolute size when greater than 100. Calculated as half of \c eviction_dirty_target
        by default. The value is not allowed to exceed the \c cache_size and has to be lower
        than its counterpart \c eviction_updates_trigger''',
        min=0, max='10TB'),
    Config('eviction_updates_trigger', '0', r'''
        trigger application threads to perform eviction when the cache contains at least this
        many bytes of updates. It is a percentage of the cache size if the value is within
        the range of 1 to 100 or an absolute size when greater than 100\. Calculated as half
        of \c eviction_dirty_trigger by default. The value is not allowed to exceed the \c
        cache_size and has to be greater than its counterpart \c eviction_updates_target. This
        setting only alters behavior if it is lower than \c eviction_trigger''',
        min=0, max='10TB'),
    Config('extra_diagnostics', '[]', r'''
        enable additional diagnostics in WiredTiger. These additional diagnostics include
        diagnostic assertions that can cause WiredTiger to abort when an invalid state
        is detected.
        Options are given as a list, such as
        <code>"extra_diagnostics=[out_of_order,visibility]"</code>.
        Choosing \c all enables all assertions. When WiredTiger is compiled with
        \c HAVE_DIAGNOSTIC=1 all assertions are enabled and cannot be reconfigured
        ''',
        type='list', choices=[
            "all", "checkpoint_validate", "cursor_check", "disk_validate", "eviction_check",
            "generation_check", "hs_validate", "key_out_of_order", "log_validate", "prepared",
            "slow_operation", "txn_visibility"]),
    Config('file_manager', '', r'''
        control how file handles are managed''',
        type='category', subconfig=[
        Config('close_handle_minimum', '250', r'''
            number of handles open before the file manager will look for handles to close''',
            min=0),
        Config('close_idle_time', '30', r'''
            amount of time in seconds a file handle needs to be idle before attempting to close
            it. A setting of 0 means that idle handles are not closed''',
            min=0, max=100000),
        Config('close_scan_interval', '10', r'''
            interval in seconds at which to check for files that are inactive and close them''',
            min=1, max=100000),
        ]),
    Config('generation_drain_timeout_ms', '240000', r'''
        the number of milliseconds to wait for a resource to drain before timing out in diagnostic
        mode. Default will wait for 4 minutes, 0 will wait forever''',
        min=0),
    Config('heuristic_controls', '', r'''
        control the behavior of various optimizations. This is primarily used as a mechanism for
        rolling out changes to internal heuristics while providing a mechanism for quickly
        reverting to prior behavior in the field''',
        type='category', subconfig=[
            Config('checkpoint_cleanup_obsolete_tw_pages_dirty_max', '100', r'''
                maximum number of obsolete time window pages that can be marked as dirty per btree
                in a single checkpoint by the checkpoint cleanup''',
                min=0, max=100000),
            Config('eviction_obsolete_tw_pages_dirty_max', '100', r'''
                maximum number of obsolete time window pages that can be marked dirty per btree in a
                single checkpoint by the eviction threads''',
                min=0, max=100000),
            Config('obsolete_tw_btree_max', '100', r'''
                maximum number of btrees that can be checked for obsolete time window cleanup in a
                single checkpoint''',
                min=0, max=500000),
        ]),
    Config('history_store', '', r'''
        history store configuration options''',
        type='category', subconfig=[
        Config('file_max', '0', r'''
            the maximum number of bytes that WiredTiger is allowed to use for its history store
            mechanism. If the history store file exceeds this size, a panic will be triggered. The
            default value means that the history store file is unbounded and may use as much
            space as the filesystem will accommodate. The minimum non-zero setting is 100MB.''',
            # !!! Must match WT_HS_FILE_MIN
            min='0')
        ]),
    Config('io_capacity', '', r'''
        control how many bytes per second are written and read. Exceeding the capacity results
        in throttling.''',
        type='category', subconfig=[
        Config('total', '0', r'''
            number of bytes per second available to all subsystems in total. When set,
            decisions about what subsystems are throttled, and in what proportion, are made
            internally. The minimum non-zero setting is 1MB.''',
            min='0', max='1TB'),
        Config('chunk_cache', '0', r'''
            number of bytes per second available to the chunk cache. The minimum non-zero setting
            is 1MB.''',
            min='0', max='1TB'),
        ]),
    Config('json_output', '[]', r'''
        enable JSON formatted messages on the event handler interface. Options are given as a
        list, where each option specifies an event handler category e.g. 'error' represents
        the messages from the WT_EVENT_HANDLER::handle_error method.''',
        type='list', choices=['error', 'message']),
    Config('operation_timeout_ms', '0', r'''
        this option is no longer supported, retained for backward compatibility.''',
        min=0),
    Config('operation_tracking', '', r'''
        enable tracking of performance-critical functions. See @ref operation_tracking for
        more information''',
        type='category', subconfig=[
            Config('enabled', 'false', r'''
                enable operation tracking subsystem''',
                type='boolean'),
            Config('path', '"."', r'''
                the name of a directory into which operation tracking files are written. The
                directory must already exist. If the value is not an absolute path, the path
                is relative to the database home (see @ref absolute_path for more information)'''),
        ]),
    Config('rollback_to_stable', '', r'''
        rollback tables to an earlier point in time, discarding all updates to checkpoint durable
        tables that have durable times more recent than the current global stable timestamp''',
        type='category', subconfig=[
            Config('threads', 4, r'''
                maximum number of threads WiredTiger will start to help RTS. Each
                RTS worker thread uses a session from the configured WT_RTS_MAX_WORKERS''',
                min=0,
                max=10),    # !!! Must match WT_RTS_MAX_WORKERS
        ]),
    Config('shared_cache', '', r'''
        shared cache configuration options. A database should configure either a cache_size
        or a shared_cache not both. Enabling a shared cache uses a session from the configured
        session_max. A shared cache can not have absolute values configured for cache eviction
        settings''',
        type='category', subconfig=[
        Config('chunk', '10MB', r'''
            the granularity that a shared cache is redistributed''',
            min='1MB', max='10TB'),
        Config('name', 'none', r'''
            the name of a cache that is shared between databases or \c "none" when no shared
            cache is configured'''),
        Config('quota', '0', r'''
            maximum size of cache this database can be allocated from the shared cache. Defaults
            to the entire shared cache size''',
            type='int'),
        Config('reserve', '0', r'''
            amount of cache this database is guaranteed to have available from the shared
            cache. This setting is per database. Defaults to the chunk size''',
            type='int'),
        Config('size', '500MB', r'''
            maximum memory to allocate for the shared cache. Setting this will update the value
            if one is already set''',
            min='1MB', max='10TB')
        ]),
    Config('statistics', 'none', r'''
        Maintain database statistics, which may impact performance. Choosing "all" maintains
        all statistics regardless of cost, "fast" maintains a subset of statistics that are
        relatively inexpensive, "none" turns off all statistics. The "clear" configuration
        resets statistics after they are gathered, where appropriate (for example, a cache size
        statistic is not cleared, while the count of cursor insert operations will be cleared).
        When "clear" is configured for the database, gathered statistics are reset each time a
        statistics cursor is used to gather statistics, as well as each time statistics are logged
        using the \c statistics_log configuration. See @ref statistics for more information''',
        type='list',
        choices=['all', 'cache_walk', 'fast', 'none', 'clear', 'tree_walk']),
    Config('timing_stress_for_test', '', r'''
        enable code that interrupts the usual timing of operations with a goal of uncovering
        race conditions and unexpected blocking. This option is intended for use with internal
        stress testing of WiredTiger.''',
        type='list', undoc=True,
        choices=[
        'aggressive_stash_free', 'aggressive_sweep', 'backup_rename', 'checkpoint_evict_page',
        'checkpoint_handle', 'checkpoint_slow', 'checkpoint_stop', 'commit_transaction_slow',
        'compact_slow', 'conn_close_stress_log_printf', 'evict_reposition',
        'failpoint_eviction_split', 'failpoint_history_store_delete_key_from_ts',
        'history_store_checkpoint_delay', 'history_store_search', 'history_store_sweep_race',
        'live_restore_clean_up', 'open_index_slow', 'prefetch_1', 'prefetch_2', 'prefetch_3',
        'prefix_compare', 'prepare_checkpoint_delay', 'prepare_resolution_1',
        'prepare_resolution_2', 'session_alter_slow', 'sleep_before_read_overflow_onpage',
        'split_1', 'split_2', 'split_3', 'split_4', 'split_5', 'split_6', 'split_7',
        'split_8','tiered_flush_finish']),
    Config('verbose', '[]', r'''
        enable messages for various subsystems and operations. Options are given as a list,
        where each message type can optionally define an associated verbosity level, such as
        <code>"verbose=[eviction,read:1,rts:0]"</code>. Verbosity levels that can be provided
        include <code>0</code> (INFO) and <code>1</code> through <code>5</code>, corresponding to
        (DEBUG_1) to (DEBUG_5). \c all is a special case that defines the verbosity level for all
        categories not explicitly set in the config string.''',
        type='list', choices=[
            'all',
            'api',
            'backup',
            'block',
            'block_cache',
            'checkpoint',
            'checkpoint_cleanup',
            'checkpoint_progress',
            'chunkcache',
            'compact',
            'compact_progress',
            'configuration',
            'disaggregated_storage',
            'error_returns',
            'eviction',
            'fileops',
            'generation',
            'handleops',
            'history_store',
            'history_store_activity',
            'layered',
            'live_restore',
            'live_restore_progress',
            'log',
            'metadata',
            'mutex',
            'out_of_order',
            'overflow',
            'page_delta',
            'prefetch',
            'read',
            'reconcile',
            'recovery',
            'recovery_progress',
            'rts',
            'salvage',
            'shared_cache',
            'split',
            'sweep',
            'temporary',
            'thread_group',
            'tiered',
            'timestamp',
            'transaction',
            'verify',
            'version',
            'write']),
]

# wiredtiger_open and WT_CONNECTION.reconfigure compatibility configurations.
compatibility_configuration_common = [
    Config('release', '', r'''
        compatibility release version string'''),
]

connection_reconfigure_compatibility_configuration = [
    Config('compatibility', '', r'''
        set compatibility version of database. Changing the compatibility version requires
        that there are no active operations for the duration of the call.''',
        type='category', subconfig=compatibility_configuration_common)
]
wiredtiger_open_compatibility_configuration = [
    Config('compatibility', '', r'''
        set compatibility version of database. Changing the compatibility version requires
        that there are no active operations for the duration of the call.''',
        type='category', subconfig=
        compatibility_configuration_common + [
        Config('require_max', '', r'''
            required maximum compatibility version of existing data files. Must be greater
            than or equal to any release version set in the \c release setting. Has no effect
            if creating the database.'''),
        Config('require_min', '', r'''
            required minimum compatibility version of existing data files. Must be less than
            or equal to any release version set in the \c release setting. Has no effect if
            creating the database.'''),
    ]),
]

# wiredtiger_open and WT_CONNECTION.reconfigure log configurations.
log_configuration_common = [
    Config('archive', 'true', r'''
        automatically remove unneeded log files (deprecated)''',
        type='boolean', undoc=True),
    Config('os_cache_dirty_pct', '0', r'''
        maximum dirty system buffer cache usage, as a percentage of the log's \c file_max.
        If non-zero, schedule writes for dirty blocks belonging to the log in the system buffer
        cache after that percentage of the log has been written into the buffer cache without
        an intervening file sync.''',
        min='0', max='100'),
    Config('prealloc', 'true', r'''
        pre-allocate log files''',
        type='boolean'),
    Config('prealloc_init_count', '1', r'''
        initial number of pre-allocated log files''',
        min='1', max='500'),
    Config('remove', 'true', r'''
        automatically remove unneeded log files''',
        type='boolean'),
    Config('zero_fill', 'false', r'''
        manually write zeroes into log files''',
        type='boolean')
]
connection_reconfigure_log_configuration = [
    Config('log', '', r'''
        enable logging. Enabling logging uses three sessions from the configured session_max''',
        type='category', subconfig=log_configuration_common)
]
wiredtiger_open_log_configuration = [
    Config('log', '', r'''
        enable logging. Enabling logging uses three sessions from the configured session_max''',
        type='category', subconfig=
        log_configuration_common + [
        Config('enabled', 'false', r'''
            enable logging subsystem''',
            type='boolean'),
        Config('compressor', 'none', r'''
            configure a compressor for log records. Permitted values are \c "none" or a custom
            compression engine name created with WT_CONNECTION::add_compressor. If WiredTiger
            has builtin support for \c "lz4", \c "snappy", \c "zlib" or \c "zstd" compression,
            these names are also available. See @ref compression for more information'''),
        Config('file_max', '100MB', r'''
            the maximum size of log files''',
            min='100KB',    # !!! Must match WT_LOG_FILE_MIN
            max='2GB'),    # !!! Must match WT_LOG_FILE_MAX
        Config('force_write_wait', '0', r'''
            enable code that interrupts the usual timing of flushing the log from the internal
            log server thread with a goal of uncovering race conditions. This option is intended
            for use with internal stress testing of WiredTiger.''',
            min='1', max='60', undoc=True),
        Config('path', '"."', r'''
            the name of a directory into which log files are written. The directory must already
            exist. If the value is not an absolute path, the path is relative to the database
            home (see @ref absolute_path for more information)'''),
        Config('recover', 'on', r'''
            run recovery or fail with an error if recovery needs to run after an unclean
            shutdown''',
            choices=['error', 'on'])
    ]),
]

# wiredtiger_open and WT_CONNECTION.reconfigure statistics log configurations.
statistics_log_configuration_common = [
    Config('json', 'false', r'''
        encode statistics in JSON format''',
        type='boolean'),
    Config('on_close', 'false', r'''log statistics on database close''',
        type='boolean'),
    Config('sources', '', r'''
        if non-empty, include statistics for the list of "file:" data source URIs,
        if they are open at the time of the statistics logging.''',
        type='list'),
    Config('timestamp', '"%b %d %H:%M:%S"', r'''
        a timestamp prepended to each log record. May contain \c strftime conversion specifications.
        When \c json is configured, defaults to \c "%Y-%m-%dT%H:%M:%S.000Z"'''),
    Config('wait', '0', r'''
        seconds to wait between each write of the log records; setting this value above 0
        configures statistics logging''',
        min='0', max='100000'),
]
connection_reconfigure_statistics_log_configuration = [
    Config('statistics_log', '', r'''
        log any statistics the database is configured to maintain, to a file. See @ref
        statistics for more information. Enabling the statistics log server uses a session from
        the configured session_max''',
        type='category', subconfig=
        statistics_log_configuration_common)
]
wiredtiger_open_statistics_log_configuration = [
    Config('statistics_log', '', r'''
        log any statistics the database is configured to maintain, to a file. See @ref
        statistics for more information. Enabling the statistics log server uses a session from
        the configured session_max''',
        type='category', subconfig=
        statistics_log_configuration_common + [
        Config('path', '"."', r'''
            the name of a directory into which statistics files are written. The directory
            must already exist. If the value is not an absolute path, the path is relative to
            the database home (see @ref absolute_path for more information)''')
        ])
]

tiered_storage_configuration_common = [
    Config('local_retention', '300', r'''
        time in seconds to retain data on tiered storage on the local tier for faster read
        access''',
        min='0', max='10000'),
]
connection_reconfigure_tiered_storage_configuration = [
    Config('tiered_storage', '', r'''
        enable tiered storage. Enabling tiered storage may use one session from the configured
        session_max''',
        type='category', subconfig=tiered_storage_configuration_common)
]
wiredtiger_open_tiered_storage_configuration = [
    Config('tiered_storage', '', r'''
        enable tiered storage. Enabling tiered storage may use one session from the configured
        session_max''',
        type='category', undoc=True, subconfig=
        tiered_storage_configuration_common + [
        Config('auth_token', '', r'''
            authentication string identifier'''),
        Config('bucket', '', r'''
            bucket string identifier where the objects should reside'''),
        Config('bucket_prefix', '', r'''
            unique string prefix to identify our objects in the bucket. Multiple instances
            can share the storage bucket and this identifier is used in naming objects'''),
        Config('cache_directory', '', r'''
            a directory to store locally cached versions of files in the storage source. By
            default, it is named with \c "-cache" appended to the bucket name. A relative
            directory name is relative to the home directory'''),
        Config('interval', '60', r'''
            interval in seconds at which to check for tiered storage related work to perform''',
            min=1, max=1000),
        Config('name', 'none', r'''
            Permitted values are \c "none" or a custom storage name created with
            WT_CONNECTION::add_storage_source'''),
        Config('shared', 'false', r'''
            enable sharing tiered tables across other WiredTiger instances.''',
            type='boolean'),
    ]),
]

# At this stage live restore intentionally does not support reconfiguring the number of worker
# threads. If that becomes necessary in the future we'll need to break out the thread count config
# and add it to the reconfigure items too. That will also introduce the need for a MAX_WORKER or
# similar macro.
wiredtiger_open_live_restore_configuration = [
    Config('live_restore', '', r'''Live restore configuration options. These options control the
    behavior of WiredTiger when live restoring from a backup.''', type='category', subconfig = [
        Config('enabled', 'false', r'''whether live restore is enabled or not.''', type='boolean'),
        Config('path', '', r'''the path to the backup that will be restored from.'''),
        Config('read_size', '1MB', r'''
            the read size for data migration, in bytes, must be a power of two. This setting is a
            best effort. It does not force every read to be this size.''', min='512B', max='16MB'),
        Config('threads_max', '8', r'''
            maximum number of threads WiredTiger will start to migrate data from the backup to the
            running WiredTiger database. Each worker thread uses a session handle from the
            configured session_max''',
            min=0, max=12)
    ])
]

chunk_cache_configuration_common = [
    Config('pinned', '', r'''
        List of "table:" URIs exempt from cache eviction. Capacity config overrides this,
        tables exceeding capacity will not be fully retained. Table names can appear
        in both this and the preload list, but not in both this and the exclude list.
        Duplicate names are allowed.''',
        type='list'),
]
connection_reconfigure_chunk_cache_configuration = [
    Config('chunk_cache', '', r'''
        chunk cache reconfiguration options''',
        type='category', subconfig=chunk_cache_configuration_common)
]
wiredtiger_open_chunk_cache_configuration = [
    Config('chunk_cache', '', r'''
        chunk cache configuration options''',
        type='category', subconfig=
        chunk_cache_configuration_common + [
        Config('capacity', '10GB', r'''
            maximum memory or storage to use for the chunk cache''',
            min='512KB', max='100TB'),
        Config('chunk_cache_evict_trigger', '90', r'''
            chunk cache percent full that triggers eviction''',
            min='0', max='100'),
        Config('chunk_size', '1MB', r'''
            size of cached chunks''',
            min='512KB', max='100GB'),
        Config('storage_path', '', r'''
            the path (absolute or relative) to the file used as cache location. This should be on a
            filesystem that supports file truncation. All filesystems in common use
            meet this criteria.'''),
        Config('enabled', 'false', r'''
            enable chunk cache''',
            type='boolean'),
        Config('hashsize', '1024', r'''
            number of buckets in the hashtable that keeps track of objects''',
            min='64', max='1048576'),
        Config('flushed_data_cache_insertion', 'true', r'''
            enable caching of freshly-flushed data, before it is removed locally.''',
            type='boolean', undoc=True),
        Config('type', 'FILE', r'''
            cache location, defaults to the file system.''',
            choices=['FILE', 'DRAM'], undoc=True),
    ]),
]

session_config = [
    Config('cache_cursors', 'true', r'''
        enable caching of cursors for reuse. Any calls to WT_CURSOR::close for a cursor created
        in this session will mark the cursor as cached and keep it available to be reused for
        later calls to WT_SESSION::open_cursor. Cached cursors may be eventually closed. This
        value is inherited from ::wiredtiger_open \c cache_cursors''',
        type='boolean'),
    Config('debug', '', r'''
        configure debug specific behavior on a session. Generally only used for internal testing
        purposes.''',
        type='category', subconfig=[
        Config('checkpoint_fail_before_turtle_update', 'false', r'''
            Fail before writing a turtle file at the end of a checkpoint.''',
            type='boolean'),
        Config('release_evict_page', 'false', r'''
            Configure the session to evict the page when it is released and no longer needed.''',
            type='boolean'),
        ]),
    Config('cache_max_wait_ms', '0', r'''
        the maximum number of milliseconds an application thread will wait for space to be
        available in cache before giving up. Default value will be the global setting of the
        connection config. 0 will wait forever. 1 will never wait''',
        min=0),
    Config('ignore_cache_size', 'false', r'''
        when set, operations performed by this session ignore the cache size and are not blocked
        when the cache is full. Note that use of this option for operations that create cache
        pressure can starve ordinary sessions that obey the cache size.''',
        type='boolean'),
    Config('isolation', 'snapshot', r'''
        the default isolation level for operations in this session''',
        choices=['read-uncommitted', 'read-committed', 'snapshot']),
    Config('prefetch', '', r'''
        Enable automatic detection of scans by applications, and attempt to pre-fetch future
        content into the cache''',
        type='category', subconfig=[
        Config('enabled', 'false', r'''
            whether pre-fetch is enabled for this session''',
            type='boolean'),
        ]),
]

wiredtiger_open_common =\
    connection_runtime_config +\
    wiredtiger_open_chunk_cache_configuration +\
    wiredtiger_open_compatibility_configuration +\
    wiredtiger_open_disaggregated_storage_configuration +\
    wiredtiger_open_page_delta_configuration +\
    wiredtiger_open_log_configuration +\
    wiredtiger_open_live_restore_configuration +\
    wiredtiger_open_tiered_storage_configuration +\
    wiredtiger_open_statistics_log_configuration + [
    Config('backup_restore_target', '', r'''
        If non-empty and restoring from a backup, restore only the table object targets listed.
        WiredTiger will remove all the metadata entries for the tables that are not listed in
        the list from the reconstructed metadata. The target list must include URIs of type
        \c table:''',
        type='list'),
    Config('buffer_alignment', '', r'''
        this option is no longer supported, retained for backward compatibility.''',
        min='-1', max='1MB', undoc=True),
    Config('builtin_extension_config', '', r'''
        A structure where the keys are the names of builtin extensions and the values are
        passed to WT_CONNECTION::load_extension as the \c config parameter (for example,
        <code>builtin_extension_config={zlib={compression_level=3}}</code>)'''),
    Config('cache_cursors', 'true', r'''
        enable caching of cursors for reuse. This is the default value for any sessions created,
        and can be overridden in configuring \c cache_cursors in WT_CONNECTION.open_session.''',
        type='boolean'),
    Config('checkpoint_sync', 'true', r'''
        flush files to stable storage when closing or writing checkpoints''',
        type='boolean'),
    Config('compile_configuration_count', '1000', r'''
        the number of configuration strings that can be precompiled. Some configuration strings
        are compiled internally when the connection is opened.''',
        min='500'),
    Config('direct_io', '', r'''
        this option is no longer supported, retained for backward compatibility.''',
        type='list', undoc=True),
    Config('encryption', '', r'''
        configure an encryptor for system wide metadata and logs. If a system wide encryptor is
        set, it is also used for encrypting data files and tables, unless encryption configuration
        is explicitly set for them when they are created with WT_SESSION::create''',
        type='category', subconfig=[
        Config('name', 'none', r'''
            Permitted values are \c "none" or a custom encryption engine name created with
            WT_CONNECTION::add_encryptor. See @ref encryption for more information'''),
        Config('keyid', '', r'''
            An identifier that identifies a unique instance of the encryptor. It is stored in
            clear text, and thus is available when the WiredTiger database is reopened. On the
            first use of a (name, keyid) combination, the WT_ENCRYPTOR::customize function is
            called with the keyid as an argument'''),
        Config('secretkey', '', r'''
            A string that is passed to the WT_ENCRYPTOR::customize function. It is never stored
            in clear text, so must be given to any subsequent ::wiredtiger_open calls to reopen the
            database. It must also be provided to any "wt" commands used with this database'''),
        ]),
    Config('extensions', '', r'''
        list of shared library extensions to load (using dlopen). Any values specified to a
        library extension are passed to WT_CONNECTION::load_extension as the \c config parameter
        (for example, <code>extensions=(/path/ext.so={entry=my_entry})</code>)''',
        type='list'),
    Config('file_extend', '', r'''
        file size extension configuration. If set, extend files of the given type in allocations of
        the given size, instead of a block at a time as each new block is written. For example,
        <code>file_extend=(data=16MB)</code>. If set to 0, disable file size extension for the
        given type. For log files, the allowed range is between 100KB and 2GB; values larger
        than the configured maximum log size and the default config would extend log files in
        allocations of the maximum log file size.''',
        type='list', choices=['data', 'log']),
    Config('hash', '', r'''
        manage resources used by hash bucket arrays. All values must be a power of two. Note that
        setting large values can significantly increase memory usage inside WiredTiger''',
        type='category', subconfig=[
        Config('buckets', 512, r'''
            configure the number of hash buckets for most system hash arrays''',
            min='64', max='65536'),
        Config('dhandle_buckets', 512, r'''
            configure the number of hash buckets for hash arrays relating to data handles''',
            min='64', max='65536'),
        ]),
    Config('hazard_max', '1000', r'''
        maximum number of simultaneous hazard pointers per session handle''',
        min=15, undoc=True),
    Config('mmap', 'true', r'''
        Use memory mapping when accessing files in a read-only mode''',
        type='boolean'),
    Config('mmap_all', 'false', r'''
        Use memory mapping to read and write all data files.''',
        type='boolean'),
    Config('multiprocess', 'false', r'''
        permit sharing between processes (will automatically start an RPC server for primary
        processes and use RPC for secondary processes). <b>Not yet supported in WiredTiger</b>''',
        type='boolean'),
    Config('prefetch', '', r'''
        Enable automatic detection of scans by applications, and attempt to pre-fetch future
        content into the cache''',
        type='category', subconfig=[
        Config('available', 'false', r'''
            whether the thread pool for the pre-fetch functionality is started''',
            type='boolean'),
        Config('default', 'false', r'''
            whether pre-fetch is enabled for all sessions by default''',
            type='boolean'),
        ]),
    Config('precise_checkpoint', 'false', r'''
            Only write data with timestamps that are smaller or equal to the stable timestamp to the
            checkpoint. Rollback to stable after restart is a no-op if enabled. However, it leads to
            extra cache pressure. The user must have set the stable timestamp. It is not compatible
            with use_timestamp=false config.''',
            type='boolean'),
    Config('preserve_prepared', 'false', r'''
        open connection in preserve prepare mode. All the prepared transactions that are
        not yet committed or rolled back will be preserved in the database. This is useful for
        applications that want to preserve prepared transactions across restarts.''',
        type='boolean'),
    Config('readonly', 'false', r'''
        open connection in read-only mode. The database must exist. All methods that may
        modify a database are disabled. See @ref readonly for more information''',
        type='boolean'),
    Config('salvage', 'false', r'''
        open connection and salvage any WiredTiger-owned database and log files that it detects as
        corrupted. This call should only be used after getting an error return of WT_TRY_SALVAGE.
        Salvage rebuilds files in place, overwriting existing files. We recommend making a
        backup copy of all files with the WiredTiger prefix prior to passing this flag.''',
        type='boolean'),
    Config('session_max', '100', r'''
        maximum expected number of sessions (including server threads)''',
        min='1'),
    Config('session_scratch_max', '2MB', r'''
        maximum memory to cache in each session''',
        type='int', undoc=True),
    Config('session_table_cache', 'true', r'''
        Maintain a per-session cache of tables''',
        type='boolean', undoc=True), # Obsolete after WT-3476
    Config('transaction_sync', '', r'''
        how to sync log records when the transaction commits''',
        type='category', subconfig=[
        Config('enabled', 'false', r'''
            whether to sync the log on every commit by default, can be overridden by the \c
            sync setting to WT_SESSION::commit_transaction''',
            type='boolean'),
        Config('method', 'fsync', r'''
            the method used to ensure log records are stable on disk, see @ref tune_durability
            for more information''',
            choices=['dsync', 'fsync', 'none']),
        ]),
    Config('verify_metadata', 'false', r'''
        open connection and verify any WiredTiger metadata. Not supported when opening a
        connection from a backup. This API allows verification and detection of corruption in
        WiredTiger metadata.''',
        type='boolean'),
    Config('write_through', '', r'''
        Use \c FILE_FLAG_WRITE_THROUGH on Windows to write to files. Ignored on non-Windows
        systems. Options are given as a list, such as <code>"write_through=[data]"</code>.
        Configuring \c write_through requires care; see @ref write_through
        Including \c "data" will cause WiredTiger data files to write through cache, including
        \c "log" will cause WiredTiger log files to write through
        cache.''',
        type='list', choices=['data', 'log']),
]

wiredtiger_open = wiredtiger_open_common + [
   Config('config_base', 'true', r'''
        write the base configuration file if creating the database. If \c false in the config
        passed directly to ::wiredtiger_open, will ignore any existing base configuration file
        in addition to not creating one. See @ref config_base for more information''',
        type='boolean'),
    Config('create', 'false', r'''
        create the database if it does not exist''',
        type='boolean'),
    Config('exclusive', 'false', r'''
        fail if the database already exists, generally used with the \c create option''',
        type='boolean'),
    Config('in_memory', 'false', r'''
        keep data in memory only. See @ref in_memory for more information''',
        type='boolean'),
    Config('use_environment', 'true', r'''
        use the \c WIREDTIGER_CONFIG and \c WIREDTIGER_HOME environment variables if the process
        is not running with special privileges. See @ref home for more information''',
        type='boolean'),
    Config('use_environment_priv', 'false', r'''
        use the \c WIREDTIGER_CONFIG and \c WIREDTIGER_HOME environment variables even if the
        process is running with special privileges. See @ref home for more information''',
        type='boolean'),
]

cursor_bound_config = [
    Config('action', 'set', r'''
        configures whether this call into the API will set or clear range bounds on the given
        cursor. It takes one of two values, "set" or "clear". If "set" is specified then "bound"
        must also be specified. The keys relevant to the given bound must have been set prior to the
        call using WT_CURSOR::set_key.''',
        choices=['clear','set']),
    Config('inclusive', 'true', r'''
        configures whether the given bound is inclusive or not.''',
        type='boolean'),
    Config('bound', '', r'''
        configures which bound is being operated on. It takes one of two values, "lower" or "upper".
        ''',
        choices=['lower','upper']),
]

cursor_runtime_config = [
    Config('append', 'false', r'''
        append written values as new records, giving each a new record number key; valid only for
        cursors with record number keys''',
        type='boolean'),
    Config('overwrite', 'true', r'''
        configures whether the cursor's insert and update methods check the existing state of
        the record. If \c overwrite is \c false, WT_CURSOR::insert fails with ::WT_DUPLICATE_KEY
        if the record exists, and WT_CURSOR::update fails with ::WT_NOTFOUND if the record does
        not exist''',
        type='boolean'),
    Config('prefix_search', 'false', r'''
        this option is no longer supported, retained for backward compatibility.''',
        type='boolean', undoc=True),
]

methods = {
'colgroup.meta' : Method(colgroup_meta),

'file.config' : Method(file_config),

'file.meta' : Method(file_meta),

'index.meta' : Method(index_meta),

'object.meta' : Method(object_meta),

'layered.meta' : Method(layered_meta),

'table.meta' : Method(table_meta),

'tier.meta' : Method(tier_meta),

'tiered.meta' : Method(tiered_meta),

'WT_CURSOR.close' : Method([]),

'WT_CURSOR.reconfigure' : Method(cursor_runtime_config),

'WT_CURSOR.bound' : Method(cursor_bound_config, compilable=True),

'WT_SESSION.alter' : Method(file_runtime_config + [
    Config('checkpoint', '', r'''
        the file checkpoint entries''',
        undoc=True),
    Config('exclusive_refreshed', 'true', r'''
        refresh the in memory state and flush the metadata change to disk, disabling this flag
        is dangerous - it will only re-write the metadata without refreshing the in-memory
        information or creating a checkpoint. The update will also only be applied to table
        URI entries in the metadata, not their sub-entries.''',
        type='boolean', undoc=True),
]),

'WT_SESSION.close' : Method([]),

'WT_SESSION.compact' : Method([
    Config('background', '', r'''
        enable/disabled the background compaction server.''',
        type='boolean'),
    Config('dryrun', 'false', r'''run only the estimation phase of compact''', type='boolean'),
    Config('exclude', '', r'''
        list of table objects to be excluded from background compaction. The list is immutable and
        only applied when the background compaction gets enabled. The list is not saved between the
        calls and needs to be reapplied each time the service is enabled. The individual objects in
        the list can only be of the \c table: URI type''',
        type='list'),
    Config('free_space_target', '20MB', r'''
        minimum amount of space recoverable for compaction to proceed''',
        min='1MB'),
    Config('run_once', 'false', r'''
        configure background compaction server to run once. In this mode, compaction is always
        attempted on each table unless explicitly excluded''',
        type='boolean'),
    Config('timeout', '1200', r'''
        maximum amount of time to allow for compact in seconds. The actual amount of time spent
        in compact may exceed the configured value. A value of zero disables the timeout''',
        type='int'),
]),

'WT_SESSION.create' : Method(file_config + lsm_config + tiered_config +\
        file_disaggregated_config + source_meta + index_only_config + table_only_config +\
        layered_config + [
    Config('exclusive', 'false', r'''
        explicitly fail with EEXIST if the object exists. When false (the default), if the object
        exists, silently fail without creating a new object.''',
        type='boolean'),
    Config('import', '', r'''
        configure import of an existing object into the currently running database''',
        type='category', subconfig=[
        Config('compare_timestamp', 'oldest_timestamp', r'''
            allow importing files with timestamps smaller or equal to the configured global
            timestamps. Note the history of the files are not imported together and thus snapshot
            read of historical data will not work with the option "stable_timestamp". (The \c
            oldest and \c stable arguments are deprecated short-hand for \c oldest_timestamp
            and \c stable_timestamp, respectively)''',
            choices=['oldest', 'oldest_timestamp', 'stable', 'stable_timestamp']),
        Config('enabled', 'false', r'''
            whether to import the input URI from disk''',
            type='boolean'),
        Config('file_metadata', '', r'''
            the file configuration extracted from the metadata of the export database'''),
        Config('metadata_file', '', r'''
            a text file that contains all the relevant metadata information for the URI to import.
            The file is generated by backup:export cursor'''),
        Config('panic_corrupt', 'true', r'''
            whether to panic if the metadata given is no longer valid for the table.
            Not valid with \c repair=true''',
            type='boolean'),
        Config('repair', 'false', r'''
            whether to reconstruct the metadata from the raw file content''',
            type='boolean'),
        ]),
]),

'WT_SESSION.drop' : Method([
    Config('checkpoint_wait', 'true', r'''
        wait for concurrent checkpoints to complete before attempting the drop operation. If
        \c checkpoint_wait=false, attempt the drop operation without waiting, returning EBUSY
        if the operation conflicts with a running checkpoint''',
        type='boolean', undoc=True),
    Config('force', 'false', r'''
        return success if the object does not exist''',
        type='boolean'),
    Config('lock_wait', 'true', r'''
        wait for locks, if \c lock_wait=false, fail if any required locks are not available
        immediately''',
        type='boolean', undoc=True),
    Config('remove_files', 'true', r'''
        if the underlying files should be removed''',
        type='boolean'),
    Config('remove_shared', 'false', r'''
        to force the removal of any shared objects. This is intended for tiered tables, and can
        only be set if the drop operation is configured to remove the underlying files. Ignore
        this configuration if it is set for non-tiered tables''',
        type='boolean', undoc=True),
]),

'WT_SESSION.log_flush' : Method([
    Config('sync', 'on', r'''
        forcibly flush the log and wait for it to achieve the synchronization level specified.
        The \c off setting forces any buffered log records to be written to the file system.
        The \c on setting forces log records to be written to the storage device''',
        choices=['off', 'on']),
]),

'WT_SESSION.log_printf' : Method([]),

'WT_SESSION.open_cursor' : Method(cursor_runtime_config + [
    Config('bulk', 'false', r'''
        configure the cursor for bulk-loading, a fast, initial load path (see @ref tune_bulk_load
        for more information). Bulk-load may only be used for newly created objects and
        applications should use the WT_CURSOR::insert method to insert rows. When bulk-loading,
        rows must be loaded in sorted order. The value is usually a true/false flag; when
        bulk-loading fixed-length column store objects, the special value \c bitmap allows
        chunks of a memory resident bitmap to be loaded directly into a file by passing a
        \c WT_ITEM to WT_CURSOR::set_value where the \c size field indicates the number of
        records in the bitmap (as specified by the object's \c value_format configuration).
        Bulk-loaded bitmap values must end on a byte boundary relative to the bit count (except
        for the last set of values loaded)'''),
    Config('checkpoint', '', r'''
        the name of a checkpoint to open. (The reserved name "WiredTigerCheckpoint" opens
        the most recent checkpoint taken for the object.) The cursor does not support data
        modification'''),
    Config('checkpoint_use_history', 'true', r'''
        when opening a checkpoint cursor, open history store cursors and retrieve snapshot and
        timestamp information from the checkpoint. This is in general required for correct reads;
        if setting it to false the caller must ensure that the checkpoint is self-contained
        in the data store: timestamps are not in use and the object was quiescent when the
        checkpoint was taken''',
        type='boolean', undoc=True),
    Config('checkpoint_wait', 'true', r'''
        wait for the checkpoint lock, if \c checkpoint_wait=false, open the cursor without
        taking a lock, returning EBUSY if the operation conflicts with a running checkpoint''',
        type='boolean', undoc=True),
    Config('debug', '', r'''
        configure debug specific behavior on a cursor. Generally only used for internal testing
        purposes''',
        type='category', subconfig=[
        Config('checkpoint_read_timestamp', '', r'''
            read the checkpoint using the specified timestamp. The supplied value must not be older
            than the checkpoint's oldest timestamp. Ignored if not reading from a checkpoint''',
            undoc=True),
        Config('dump_version', 'false', r'''
            open a version cursor, which is a debug cursor on a table that enables iteration
            through the history of values for all the keys.''',
            type='category', subconfig=[
                Config('enabled', 'false', r'''
                    enable version cursor''',
                    type='boolean', undoc=True),
                Config('visible_only', 'false', r'''
                    only dump updates that are visible to the session''',
                    type='boolean', undoc=True),
                Config('start_timestamp', '', r'''
                    Only return updates with durable timestamps larger than the start timestamp. If
                    a tombstone has a timestamp larger than the start timestamp but the associated
                    full value has a timestamp smaller than the start timestamp, it returns the
                    tombstone and the full value.''', undoc=True),
                Config('timestamp_order', 'false', r'''
                    Return the updates in timestamp order from newest to oldest and ignore duplicate
                    updates and updates that are from the same transaction with the same timestamp.
                    ''',
                    type='boolean', undoc=True),
                Config('raw_key_value', 'false', r'''
                    Return the key, value as raw data.
                    ''',
                    type='boolean', undoc=True),
        ]),
        Config('release_evict', 'false', r'''
            Configure the cursor to evict the page positioned on when the reset API call is used''',
            type='boolean'),
        ]),
    Config('dump', '', r'''
        configure the cursor for dump format inputs and outputs: "hex" selects a simple hexadecimal
        format, "json" selects a JSON format with each record formatted as fields named by column
        names if available, "pretty" selects a human-readable format (making it incompatible
        with the "load"), "pretty_hex" is similar to "pretty" (also incompatible with "load")
        except raw byte data elements will be printed like "hex" format, and "print" selects
        a format where only non-printing characters are hexadecimal encoded. These formats are
        compatible with the @ref util_dump and @ref util_load commands''',
        choices=['hex', 'json', 'pretty', 'pretty_hex', 'print']),
    Config('incremental', '', r'''
        configure the cursor for block incremental backup usage. These formats are only compatible
        with the backup data source; see @ref backup''',
        type='category', subconfig=[
        Config('consolidate', 'false', r'''
            causes block incremental backup information to be consolidated if adjacent granularity
            blocks are modified. If false, information will be returned in granularity sized
            blocks only. This must be set on the primary backup cursor and it applies to all
            files for this backup''',
            type='boolean'),
        Config('enabled', 'false', r'''
            whether to configure this backup as the starting point for a subsequent incremental
            backup''',
            type='boolean'),
        Config('file', '', r'''
            the file name when opening a duplicate incremental backup cursor. That duplicate
            cursor will return the block modifications relevant to the given file name'''),
        Config('force_stop', 'false', r'''
            causes all block incremental backup information to be released. This is on an
            open_cursor call and the resources will be released when this cursor is closed. No
            other operations should be done on this open cursor''',
            type='boolean'),
        Config('granularity', '16MB', r'''
            this setting manages the granularity of how WiredTiger maintains modification maps
            internally. The larger the granularity, the smaller amount of information WiredTiger
            need to maintain''',
            min='4KB', max='2GB'),
        Config('src_id', '', r'''
            a string that identifies a previous checkpoint backup source as the source of this
            incremental backup. This identifier must have already been created by use of the
            'this_id' configuration in an earlier backup. A source id is required to begin an
            incremental backup'''),
        Config('this_id', '', r'''
            a string that identifies the current system state  as a future backup source for
            an incremental backup via \c src_id. This identifier is required when opening an
            incremental backup cursor and an error will be returned if one is not provided.
            The identifiers can be any text string, but should be unique'''),
        ]),
    Config('next_random', 'false', r'''
        configure the cursor to return a pseudo-random record from the object when the
        WT_CURSOR::next method is called; valid only for row-store cursors. See @ref cursor_random
        for details''',
        type='boolean'),
    Config('next_random_sample_size', '0', r'''
        cursors configured by \c next_random to return pseudo-random records from the object
        randomly select from the entire object, by default. Setting \c next_random_sample_size
        to a non-zero value sets the number of samples the application expects to take
        using the \c next_random cursor. A cursor configured with both \c next_random and \c
        next_random_sample_size attempts to divide the object into \c next_random_sample_size
        equal-sized pieces, and each retrieval returns a record from one of those pieces. See
        @ref cursor_random for details'''),
    Config('next_random_seed', '0', r'''
        configure the cursor to set an initial random seed when using \c next_random configuration.
        This is used for testing purposes only. See @ref cursor_random for details'''),
    Config('raw', 'false', r'''
        ignore the encodings for the key and value, manage data as if the formats were \c "u".
        See @ref cursor_raw for details''',
        type='boolean'),
    Config('read_once', 'false', r'''
        results that are brought into cache from disk by this cursor will be given less priority
        in the cache.''',
        type='boolean'),
    Config('readonly', 'false', r'''
        only query operations are supported by this cursor. An error is returned if a modification
        is attempted using the cursor. The default is false for all cursor types except for
        metadata cursors and checkpoint cursors''',
        type='boolean'),
    Config('skip_sort_check', 'false', r'''
        skip the check of the sort order of each bulk-loaded key''',
        type='boolean', undoc=True),
    Config('statistics', '', r'''
        Specify the statistics to be gathered. Choosing "all" gathers statistics regardless of
        cost and may include traversing on-disk files; "fast" gathers a subset of relatively
        inexpensive statistics. The selection must agree with the database \c statistics
        configuration specified to ::wiredtiger_open or WT_CONNECTION::reconfigure. For example,
        "all" or "fast" can be configured when the database is configured with "all", but
        the cursor open will fail if "all" is specified when the database is configured with
        "fast", and the cursor open will fail in all cases when the database is configured
        with "none". If "size" is configured, only the underlying size of the object on
        disk is filled in and the object is not opened. If \c statistics is not configured,
        the default configuration is the database configuration. The "clear" configuration
        resets statistics after gathering them, where appropriate (for example, a cache size
        statistic is not cleared, while the count of cursor insert operations will be cleared).
        See @ref statistics for more information''',
        type='list', choices=['all', 'cache_walk', 'fast', 'clear', 'size', 'tree_walk']),
    Config('target', '', r'''
        if non-empty, back up the given list of objects; valid only for a backup data source''',
        type='list'),
]),

'WT_SESSION.query_timestamp' : Method([
    Config('get', 'read', r'''
        specify which timestamp to query: \c commit returns the most recently set commit_timestamp;
        \c first_commit returns the first set commit_timestamp; \c prepare returns the timestamp
        used in preparing a transaction; \c read returns the timestamp at which the transaction
        is reading. See @ref timestamp_txn_api''',
        choices=['commit', 'first_commit', 'prepare', 'read']),
]),

'WT_SESSION.reset_snapshot' : Method([]),
'WT_SESSION.reset' : Method([]),
'WT_SESSION.salvage' : Method([
    Config('force', 'false', r'''
        force salvage even of files that do not appear to be WiredTiger files''',
        type='boolean'),
]),

'WT_SESSION.strerror' : Method([]),

'WT_SESSION.truncate' : Method([]),
'WT_SESSION.verify' : Method([
    Config('do_not_clear_txn_id', 'false', r'''
        Turn off transaction id clearing, intended for debugging and better diagnosis of crashes
        or failures. Note: History store validation is disabled when the configuration is set as
        visibility rules may not work correctly because the transaction ids are not cleared.''',
        type='boolean'),
    Config('dump_address', 'false', r'''
        Display page addresses, time windows, and page types as pages are verified, using the
        application's message handler, intended for debugging''',
        type='boolean'),
    Config('dump_all_data', 'false', r'''
        Display application data as pages or blocks are verified, using the application's message
        handler, intended for debugging. Disabling this does not guarantee that no user data will
        be output''',
        type='boolean'),
    Config('dump_key_data', 'false', r'''
        Display application data keys as pages or blocks are verified, using the application's
        message handler, intended for debugging. Disabling this does not guarantee that no user
        data will be output''',
        type='boolean'),
    Config('dump_blocks', 'false', r'''
        Display the contents of on-disk blocks as they are verified, using the application's
        message handler, intended for debugging''',
        type='boolean'),
    Config('dump_layout', 'false', r'''
        Display the layout of the files as they are verified, using the application's message
        handler, intended for debugging; requires optional support from the block manager''',
        type='boolean'),
    Config('dump_tree_shape', 'false', r'''
        Display the btree shapes as they are verified, using the application's message
        handler, intended for debugging; requires optional support from the block manager''',
        type='boolean'),
    Config('dump_offsets', '', r'''
        Display the contents of specific on-disk blocks, using the application's message handler,
        intended for debugging''',
        type='list'),
    Config('dump_pages', 'false', r'''
        Display the contents of in-memory pages as they are verified, using the application's
        message handler, intended for debugging''',
        type='boolean'),
    Config('read_corrupt', 'false', r'''
        A mode that allows verify to continue reading after encountering a checksum error. It
        will skip past the corrupt block and continue with the verification process''',
        type='boolean'),
    Config('stable_timestamp', 'false', r'''
        Ensure that no data has a start timestamp after the stable timestamp, to be run after
        rollback_to_stable.''',
        type='boolean'),
    Config('strict', 'false', r'''
        Treat any verification problem as an error; by default, verify will warn, but not fail,
        in the case of errors that won't affect future behavior (for example, a leaked block)''',
        type='boolean'),
]),

'WT_SESSION.begin_transaction' : Method([
    Config('ignore_prepare', 'false', r'''
        whether to ignore updates by other prepared transactions when doing of read operations
        of this transaction. When \c true, forces the transaction to be read-only. Use \c force
        to ignore prepared updates and permit writes (see @ref timestamp_prepare_ignore_prepare
        for more information)''',
        choices=['false', 'force', 'true']),
    Config('isolation', '', r'''
        the isolation level for this transaction; defaults to the session's isolation level''',
        choices=['read-uncommitted', 'read-committed', 'snapshot']),
    Config('name', '', r'''
        name of the transaction for tracing and debugging'''),
    Config('no_timestamp', 'false', r'''
        allow a commit without a timestamp, creating values that have "always existed" and are
        visible regardless of timestamp. See @ref timestamp_txn_api''',
        type='boolean'),
    Config('operation_timeout_ms', '0', r'''
        when non-zero, a requested limit on the time taken to complete operations in this
        transaction. Time is measured in real time milliseconds from the start of each WiredTiger
        API call. There is no guarantee any operation will not take longer than this amount of time.
        If WiredTiger notices the limit has been exceeded, an operation may return a WT_ROLLBACK
        error. Default is to have no limit''',
        min=0),
    Config('priority', 0, r'''
        priority of the transaction for resolving conflicts. Transactions with higher values
        are less likely to abort''',
        min='-100', max='100'),
    Config('read_timestamp', '', r'''
        read using the specified timestamp. The value must not be older than the current oldest
        timestamp. See @ref timestamp_txn_api'''),
    Config('roundup_timestamps', '', r'''
        round up timestamps of the transaction''',
        type='category', subconfig= [
        Config('prepared', 'false', r'''
            applicable only for prepared transactions, and intended only for special-purpose use.
            See @ref timestamp_prepare_roundup. Allows the prepare timestamp and the commit
            timestamp of this transaction to be rounded up to be no older than the oldest
            timestamp, and allows violating the usual restriction that the prepare timestamp
            must be newer than the stable timestamp. Specifically: at transaction prepare, if
            the prepare timestamp is less than or equal to the oldest timestamp, the prepare
            timestamp will be rounded to the oldest timestamp. Subsequently, at commit time,
            if the commit timestamp is less than the (now rounded) prepare timestamp, the commit
            timestamp will be rounded up to it and thus to at least oldest. Neither timestamp
            will be checked against the stable timestamp''',
            type='boolean'),
        Config('read', 'false', r'''
            if the read timestamp is less than the oldest timestamp, the read timestamp will be
            rounded up to the oldest timestamp. See @ref timestamp_read_roundup''',
            type='boolean'),
        ]),
    Config('sync', '', r'''
        whether to sync log records when the transaction commits, inherited from ::wiredtiger_open
        \c transaction_sync''',
        type='boolean'),
    Config('claim_prepared_id', '', r'''
        allow a session to claim a prepared transaction that was restored upon restart by
        specifying the transaction's prepared ID. Returns WT_NOTFOUND if the prepared id doesn't
        exist.''')
], compilable=True),

'WT_SESSION.commit_transaction' : Method([
    Config('commit_timestamp', '', r'''
        set the commit timestamp for the current transaction. For non-prepared transactions,
        the value must not be older than the first commit timestamp already set for the current
        transaction (if any), must not be older than the current oldest timestamp, and must
        be after the current stable timestamp. For prepared transactions, a commit timestamp
        is required, must not be older than the prepare timestamp, and can be set only once.
        See @ref timestamp_txn_api and @ref timestamp_prepare'''),
    Config('durable_timestamp', '', r'''
        set the durable timestamp for the current transaction. Required for the commit of a
        prepared transaction, and otherwise not permitted. The value must also be after the
        current oldest and stable timestamps and must not be older than the commit timestamp.
        See @ref timestamp_prepare'''),
    Config('operation_timeout_ms', '0', r'''
        when non-zero, a requested limit on the time taken to complete operations in this
        transaction. Time is measured in real time milliseconds from the start of each WiredTiger
        API call. There is no guarantee any operation will not take longer than this amount of time.
        If WiredTiger notices the limit has been exceeded, an operation may return a WT_ROLLBACK
        error. Default is to have no limit''',
        min=0),
    Config('sync', '', r'''
        override whether to sync log records when the transaction commits. The default is inherited
        from ::wiredtiger_open \c transaction_sync. The \c off setting does not wait for records
        to be written or synchronized. The \c on setting forces log records to be written to
        the storage device''',
        choices=['off', 'on']),
]),

'WT_SESSION.prepare_transaction' : Method([
    Config('prepare_timestamp', '', r'''
        set the prepare timestamp for the updates of the current transaction. The value must
        not be older than any active read timestamps, and must be newer than the current stable
        timestamp. See @ref timestamp_prepare'''),
    Config('prepared_id', '', r'''
        set the optional prepared ID for the prepared updates of the current transaction. Multiple
        transactions can share a prepared ID, as long as they are all guaranteed to share a decision
        whether to commit or abort and share the same prepare, commit and durable timestamps. It is
        ignored if the preserve prepared config is not enabled.''')
]),

'WT_SESSION.timestamp_transaction_uint' : Method([]),
'WT_SESSION.timestamp_transaction' : Method([
    Config('commit_timestamp', '', r'''
        set the commit timestamp for the current transaction. For non-prepared transactions,
        the value must not be older than the first commit timestamp already set for the current
        transaction, if any, must not be older than the current oldest timestamp and must be after
        the current stable timestamp. For prepared transactions, a commit timestamp is required,
        must not be older than the prepare timestamp, can be set only once, and must not be
        set until after the transaction has successfully prepared. See @ref timestamp_txn_api
        and @ref timestamp_prepare'''),
    Config('durable_timestamp', '', r'''
        set the durable timestamp for the current transaction. Required for the commit of a
        prepared transaction, and otherwise not permitted. Can only be set after the transaction
        has been prepared and a commit timestamp has been set. The value must be after the
        current oldest and stable timestamps and must not be older than the commit timestamp. See
        @ref timestamp_prepare'''),
    Config('prepare_timestamp', '', r'''
        set the prepare timestamp for the updates of the current transaction. The value must
        not be older than any active read timestamps, and must be newer than the current stable
        timestamp. Can be set only once per transaction. Setting the prepare timestamp does
        not by itself prepare the transaction, but does oblige the application to eventually
        prepare the transaction before committing it. See @ref timestamp_prepare'''),
    Config('read_timestamp', '', r'''
        read using the specified timestamp. The value must not be older than the current oldest
        timestamp. This can only be set once for a transaction. See @ref timestamp_txn_api'''),
    Config('rollback_timestamp', '', r'''
        set the rollback timestamp for the current transaction. This is valid only for prepared
        transactions under the preserve_prepared config. For prepared transactions, a rollback
        timestamp is required, must not be older than the prepare timestamp, and can be set only
        once. It is ignored if the preserve prepared config is not enabled. See
        @ref timestamp_txn_api and @ref timestamp_prepare''')
]),

'WT_SESSION.prepared_id_transaction_uint' : Method([]),
'WT_SESSION.prepared_id_transaction' : Method([
    Config('prepared_id', '', r'''
        set the optional prepared ID for the prepared updates of the current transaction. Multiple
        transactions can share a prepared ID, as long as they are all guaranteed to share a decision
        whether to commit or abort and share the same prepare, commit and durable timestamps. It is
        ignored if the preserve prepared config is not enabled.''')
]),

'WT_SESSION.rollback_transaction' : Method([
    Config('operation_timeout_ms', '0', r'''
        when non-zero, a requested limit on the time taken to complete operations in this
        transaction. Time is measured in real time milliseconds from the start of each WiredTiger
        API call. There is no guarantee any operation will not take longer than this amount of time.
        If WiredTiger notices the limit has been exceeded, an operation may return a WT_ROLLBACK
        error. Default is to have no limit''',
        min=0),
    Config('rollback_timestamp', '', r'''
        set the rollback timestamp for the current transaction. This is valid only for prepared
        transactions under the preserve_prepared config. For prepared transactions, a rollback
        timestamp is required, must not be older than the prepare timestamp, and can be set only
        once. See @ref timestamp_txn_api and @ref timestamp_prepare'''),
]),

'WT_SESSION.checkpoint' : Method([
    Config('debug', '', r'''
        configure debug specific behavior on a checkpoint. Generally only used for internal testing
        purposes''',
        type='category', subconfig=[
        Config('checkpoint_cleanup', 'false', r'''
            if true, checkpoint cleanup thread is triggered to perform the checkpoint cleanup''',
            type='boolean'),
        Config('checkpoint_crash_point', '-1', r'''
            non-negative number between 0 and 1000 will trigger a controlled crash during the
            checkpoint process. Lower values will trigger crashes in the initial phase of
            checkpoint, while higher values will result in crashes in the final phase of the
            checkpoint process''',
            type='int'),
        ]),
    Config('drop', '', r'''
        specify a list of checkpoints to drop. The list may additionally contain one of the
        following keys: \c "from=all" to drop all checkpoints, \c "from=<checkpoint>" to drop
        all checkpoints after and including the named checkpoint, or \c "to=<checkpoint>" to
        drop all checkpoints before and including the named checkpoint. Checkpoints cannot be
        dropped if open in a cursor. While a hot backup is in progress, checkpoints created
        prior to the start of the backup cannot be dropped''',
        type='list'),
    Config('flush_tier', '', r'''
        configure flushing objects to tiered storage after checkpoint. See @ref tiered_storage''',
        type='category', subconfig= [
            Config('enabled', 'false', r'''
                if true and tiered storage is in use, perform one iteration of object switching
                and flushing objects to tiered storage''',
                type='boolean'),
            Config('force', 'false', r'''
                if false (the default), flush_tier of any individual object may be skipped if the
                underlying object has not been modified since the previous flush_tier. If true,
                this option forces the flush_tier''',
                type='boolean'),
            Config('sync', 'true', r'''
                wait for all objects to be flushed to the shared storage to the level specified.
                When false, do not wait for any objects to be written to the tiered storage system
                but return immediately after generating the objects and work units for an internal
                thread.  When true, the caller waits until all work queued for this call to be
                completely processed before returning''',
                type='boolean'),
            Config('timeout', '0', r'''
                amount of time, in seconds, to wait for flushing of objects to complete.
                WiredTiger returns EBUSY if the timeout is reached. A value of zero disables
                the timeout''',
                type='int'),
    ]),
    Config('force', 'false', r'''
        if false (the default), checkpoints may be skipped if the underlying object has not been
        modified. If true, this option forces the checkpoint''',
        type='boolean'),
    Config('name', '', r'''
        if set, specify a name for the checkpoint'''),
    Config('use_timestamp', 'true', r'''
        if true (the default), create the checkpoint as of the last stable timestamp if timestamps
        are in use, or with all committed  updates if there is no stable timestamp set. If false,
        always generate a checkpoint with all committed updates, ignoring any stable timestamp''',
        type='boolean'),
]),

'WT_CONNECTION.add_collator' : Method([]),
'WT_CONNECTION.add_compressor' : Method([]),
'WT_CONNECTION.add_data_source' : Method([]),
'WT_CONNECTION.add_encryptor' : Method([]),
'WT_CONNECTION.add_page_log' : Method([]),
'WT_CONNECTION.add_storage_source' : Method([]),
'WT_CONNECTION.close' : Method([
    Config('final_flush', 'false', r'''
        wait for final flush_tier to copy objects''',
        type='boolean', undoc=True),
    Config('leak_memory', 'false', r'''
        don't free memory during close''',
        type='boolean'),
    Config('use_timestamp', 'true', r'''
        by default, create the close checkpoint as of the last stable timestamp if timestamps
        are in use, or all current updates if there is no stable timestamp set. If false, this
        option generates a checkpoint with all updates''',
        type='boolean'),
]),

'WT_CONNECTION.debug_info' : Method([
    Config('backup', 'false', r'''
        print incremental backup information''', type='boolean'),
    Config('cache', 'false', r'''
        print cache information''', type='boolean'),
    Config('cursors', 'false', r'''
        print all open cursor information''', type='boolean'),
    Config('handles', 'false', r'''
        print open handles information''', type='boolean'),
    Config('log', 'false', r'''
        print log information''', type='boolean'),
    Config('metadata', 'false', r'''
        print metadata information''', type='boolean'),
    Config('sessions', 'false', r'''
        print open session information''', type='boolean'),
    Config('txn', 'false', r'''
        print global txn information''', type='boolean'),
]),
'WT_CONNECTION.reconfigure' : Method(
    connection_reconfigure_chunk_cache_configuration +\
    connection_reconfigure_compatibility_configuration +\
    connection_reconfigure_disaggregated_configuration +\
    connection_reconfigure_page_delta_configuration +\
    connection_reconfigure_log_configuration +\
    connection_reconfigure_statistics_log_configuration +\
    connection_reconfigure_tiered_storage_configuration +\
    connection_runtime_config
),
'WT_CONNECTION.set_file_system' : Method([]),

'WT_CONNECTION.load_extension' : Method([
    Config('config', '', r'''
        configuration string passed to the entry point of the extension as its WT_CONFIG_ARG
        argument'''),
    Config('early_load', 'false', r'''
        whether this extension should be loaded at the beginning of ::wiredtiger_open. Only
        applicable to extensions loaded via the wiredtiger_open configurations string''',
        type='boolean'),
    Config('entry', 'wiredtiger_extension_init', r'''
        the entry point of the extension, called to initialize the extension when it is loaded.
        The signature of the function must match ::wiredtiger_extension_init'''),
    Config('terminate', 'wiredtiger_extension_terminate', r'''
        an optional function in the extension that is called before the extension is unloaded
        during WT_CONNECTION::close. The signature of the function must match
        ::wiredtiger_extension_terminate'''),
]),

'WT_CONNECTION.open_session' : Method(session_config),

'WT_CONNECTION.query_timestamp' : Method([
    Config('get', 'all_durable', r'''
        specify which timestamp to query: \c all_durable returns the largest timestamp such
        that all timestamps up to and including that value have been committed (possibly
        bounded by the application-set \c durable timestamp); \c backup_checkpoint returns
        the stable timestamp of the checkpoint pinned for an open backup cursor; \c last_checkpoint
        returns the timestamp of the most recent stable checkpoint; \c oldest_timestamp returns the
        most recent \c oldest_timestamp set with WT_CONNECTION::set_timestamp; \c oldest_reader
        returns the minimum of the read timestamps of all active readers; \c pinned returns
        the minimum of the \c oldest_timestamp and the read timestamps of all active readers;
        \c recovery returns the timestamp of the most recent stable checkpoint taken prior to
        a shutdown; \c stable_timestamp returns the most recent \c stable_timestamp set with
        WT_CONNECTION::set_timestamp. (The \c oldest and \c stable arguments are deprecated
        short-hand for \c oldest_timestamp and \c stable_timestamp, respectively.) See @ref
        timestamp_global_api''',
        choices=['all_durable','backup_checkpoint','last_checkpoint','oldest',
            'oldest_reader','oldest_timestamp','pinned','recovery','stable','stable_timestamp']),
]),

'WT_CONNECTION.set_timestamp' : Method([
    Config('durable_timestamp', '', r'''
        temporarily set the system's maximum durable timestamp, bounding the timestamp returned
        by WT_CONNECTION::query_timestamp with the \c all_durable configuration. Calls to
        WT_CONNECTION::query_timestamp will ignore durable timestamps greater than the specified
        value until a subsequent transaction commit advances the maximum durable timestamp, or
        rollback-to-stable resets the value. See @ref timestamp_global_api'''),
    Config('force', 'false', r'''
        set the oldest and stable timestamps even if it violates normal ordering constraints.''',
        type='boolean', undoc=True),
    Config('oldest_timestamp', '', r'''
        future commits and queries will be no earlier than the specified timestamp. Values must
        be monotonically increasing. The value must not be newer than the current stable timestamp.
        See @ref timestamp_global_api'''),
    Config('stable_timestamp', '', r'''
        checkpoints will not include commits that are newer than the specified timestamp in tables
        configured with \c "log=(enabled=false)". Values must be monotonically increasing. The value
        must not be older than the current oldest timestamp. See @ref timestamp_global_api'''),
]),

'WT_CONNECTION.rollback_to_stable' : Method([
    Config('dryrun', 'false', r'''
        perform the checks associated with RTS, but don't modify any data.''',
        type='boolean'),
    Config('threads', '4', r'''
        maximum number of threads WiredTiger will start to help RTS. Each
        RTS worker thread uses a session from the configured WT_RTS_MAX_WORKERS''',
        min=0,
        max=10),     # !!! Must match WT_RTS_MAX_WORKERS
]),

'WT_SESSION.reconfigure' : Method(session_config),

# There are 4 variants of the wiredtiger_open configurations.
# wiredtiger_open:
#    Configuration values allowed in the application's configuration
#    argument to the wiredtiger_open call.
# wiredtiger_open_basecfg:
#    Configuration values allowed in the WiredTiger.basecfg file (remove
# creation-specific configuration strings and add a version string).
# wiredtiger_open_usercfg:
#    Configuration values allowed in the WiredTiger.config file (remove
# creation-specific configuration strings).
# wiredtiger_open_all:
#    All of the above configuration values combined
'wiredtiger_open' : Method(wiredtiger_open),
'wiredtiger_open_basecfg' : Method(wiredtiger_open_common + [
    Config('version', '(major=0,minor=0)', r'''
        the file version'''),
]),
'wiredtiger_open_usercfg' : Method(wiredtiger_open_common),
'wiredtiger_open_all' : Method(wiredtiger_open + [
    Config('version', '(major=0,minor=0)', r'''
        the file version'''),
]),
}
