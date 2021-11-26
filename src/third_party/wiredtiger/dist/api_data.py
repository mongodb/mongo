# This file is a python script that describes the WiredTiger API.

class Method:
    def __init__(self, config):
        # Deal with duplicates: with complex configurations (like
        # WT_SESSION::create), it's simpler to deal with duplicates once than
        # manually as configurations are defined
        self.config = []
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
        enable enhanced checking. ''',
        type='category', subconfig= [
        Config('commit_timestamp', 'none', r'''
            This option is no longer supported, retained for backward compatibility''',
            choices=['always', 'key_consistent', 'never', 'none']),
        Config('durable_timestamp', 'none', r'''
            This option is no longer supported, retained for backward compatibility''',
            choices=['always', 'key_consistent', 'never', 'none']),
        Config('write_timestamp', 'off', r'''
            verify that commit timestamps are used per the configured
            \c write_timestamp_usage option for this table''',
            choices=['off', 'on']),
        Config('read_timestamp', 'none', r'''
            verify that timestamps should \c always or \c never be used
            on reads with this table.  Verification is \c none
            if mixed read use is allowed''',
            choices=['always', 'never', 'none'])
        ], undoc=True),
    Config('verbose', '[]', r'''
        enable messages for various events. Options are given as a
        list, such as <code>"verbose=[write_timestamp]"</code>''',
        type='list', choices=[
            'write_timestamp',
        ]),
    Config('write_timestamp_usage', 'none', r'''
        describe how timestamps are expected to be used on modifications to
        the table. This option should be used in conjunction with the
        corresponding \c write_timestamp configuration under the \c assert and
        \c verbose options to provide logging and assertions for incorrect
        timestamp usage. The choices are \c always which ensures a timestamp is
        used for every operation on a table, \c key_consistent to ensure that
        once timestamps are used for a key, they are always used, \c ordered is
        like \c key_consistent except it also enforces that subsequent updates
        to each key must use increasing timestamps, \c mixed_mode is like
        \c ordered except that updates with no timestamp are allowed and have
        the effect of resetting the chain of updates once the transaction ID
        based snapshot is no longer relevant, \c never enforces that timestamps
        are never used for a table and \c none does not enforce any expectation
        on timestamp usage meaning that no log message or assertions will be
        produced regardless of the corresponding \c assert and \c verbose
        settings''',
        choices=['always', 'key_consistent', 'mixed_mode', 'never', 'none', 'ordered']),
]

# Metadata shared by all schema objects
common_meta = common_runtime_config + [
    Config('collator', 'none', r'''
        configure custom collation for keys.  Permitted values are \c "none"
        or a custom collator name created with WT_CONNECTION::add_collator'''),
    Config('columns', '', r'''
        list of the column names.  Comma-separated list of the form
        <code>(column[,...])</code>.  For tables, the number of entries
        must match the total number of values in \c key_format and \c
        value_format.  For colgroups and indices, all column names must
        appear in the list of columns for the table''',
        type='list'),
]

source_meta = [
    Config('source', '', r'''
        set a custom data source URI for a column group, index or simple
        table.  By default, the data source URI is derived from the \c
        type and the column group or index name.  Applications can
        create tables from existing data sources by supplying a \c
        source configuration''', undoc=True),
    Config('type', 'file', r'''
        set the type of data source used to store a column group, index
        or simple table.  By default, a \c "file:" URI is derived from
        the object name.  The \c type configuration can be used to
        switch to a different data source, such as LSM or an extension
        configured by the application'''),
]

format_meta = common_meta + [
    Config('key_format', 'u', r'''
        the format of the data packed into key items.  See @ref
        schema_format_types for details.  By default, the key_format is
        \c 'u' and applications use WT_ITEM structures to manipulate
        raw byte arrays. By default, records are stored in row-store
        files: keys of type \c 'r' are record numbers and records
        referenced by record number are stored in column-store files''',
        type='format', func='__wt_struct_confchk'),
    Config('value_format', 'u', r'''
        the format of the data packed into value items.  See @ref
        schema_format_types for details.  By default, the value_format
        is \c 'u' and applications use a WT_ITEM structure to
        manipulate raw byte arrays. Value items of type 't' are
        bitfields, and when configured with record number type keys,
        will be stored using a fixed-length store''',
        type='format', func='__wt_struct_confchk'),
]

lsm_config = [
    Config('lsm', '', r'''
        options only relevant for LSM data sources''',
        type='category', subconfig=[
        Config('auto_throttle', 'true', r'''
            Throttle inserts into LSM trees if flushing to disk isn't
            keeping up''',
            type='boolean'),
        Config('bloom', 'true', r'''
            create bloom filters on LSM tree chunks as they are merged''',
            type='boolean'),
        Config('bloom_bit_count', '16', r'''
            the number of bits used per item for LSM bloom filters''',
            min='2', max='1000'),
        Config('bloom_config', '', r'''
            config string used when creating Bloom filter files, passed
            to WT_SESSION::create'''),
        Config('bloom_hash_count', '8', r'''
            the number of hash values per item used for LSM bloom
            filters''',
            min='2', max='100'),
        Config('bloom_oldest', 'false', r'''
            create a bloom filter on the oldest LSM tree chunk. Only
            supported if bloom filters are enabled''',
            type='boolean'),
        Config('chunk_count_limit', '0', r'''
            the maximum number of chunks to allow in an LSM tree. This
            option automatically times out old data. As new chunks are
            added old chunks will be removed. Enabling this option
            disables LSM background merges''',
            type='int'),
        Config('chunk_max', '5GB', r'''
            the maximum size a single chunk can be. Chunks larger than this
            size are not considered for further merges. This is a soft
            limit, and chunks larger than this value can be created.  Must
            be larger than chunk_size''',
            min='100MB', max='10TB'),
        Config('chunk_size', '10MB', r'''
            the maximum size of the in-memory chunk of an LSM tree.  This
            limit is soft - it is possible for chunks to be temporarily
            larger than this value.  This overrides the \c memory_page_max
            setting''',
            min='512K', max='500MB'),
        Config('merge_custom', '', r'''
            configure the tree to merge into a custom data source''',
            type='category', subconfig=[
            Config('prefix', '', r'''
                custom data source prefix instead of \c "file"'''),
            Config('start_generation', '0', r'''
                merge generation at which the custom data source is used
                (zero indicates no custom data source)''',
                min='0', max='10'),
            Config('suffix', '', r'''
                custom data source suffix instead of \c ".lsm"'''),
            ]),
        Config('merge_max', '15', r'''
            the maximum number of chunks to include in a merge operation''',
            min='2', max='100'),
        Config('merge_min', '0', r'''
            the minimum number of chunks to include in a merge operation. If
            set to 0 or 1 half the value of merge_max is used''',
            max='100'),
    ]),
]

tiered_config = [
    Config('tiered_storage', '', r'''
        configure a storage source for this table''',
        type='category', subconfig=[
        Config('name', 'none', r'''
            permitted values are \c "none"
            or custom storage source name created with
            WT_CONNECTION::add_storage_source.
            See @ref custom_storage_sources for more information'''),
        Config('auth_token', '', r'''
            authentication string identifier'''),
        Config('bucket', '', r'''
            the bucket indicating the location for this table'''),
        Config('bucket_prefix', '', r'''
            the unique bucket prefix for this table'''),
        Config('cache_directory', '', r'''
            a directory to store locally cached versions of files in the storage source.  By
            default, it is named with \c "-cache" appended to the bucket name.  A relative
            directory name is relative to the home directory'''),
        Config('local_retention', '300', r'''
            time in seconds to retain data on tiered storage on the local tier
            for faster read access''',
            min='0', max='10000'),
        Config('object_target_size', '10M', r'''
            the approximate size of objects before creating them on the
            tiered storage tier''',
            min='100K', max='10TB'),
        ]),
]

tiered_tree_config = [
    Config('bucket', '', r'''
        the bucket indicating the location for this table'''),
    Config('bucket_prefix', '', r'''
        the unique bucket prefix for this table'''),
    Config('cache_directory', '', r'''
        a directory to store locally cached versions of files in the storage source.  By
        default, it is named with \c "-cache" appended to the bucket name.  A relative
        directory name is relative to the home directory'''),
]

file_runtime_config = common_runtime_config + [
    Config('access_pattern_hint', 'none', r'''
        It is recommended that workloads that consist primarily of
        updates and/or point queries specify \c random.  Workloads that
        do many cursor scans through large ranges of data specify
        \c sequential and other workloads specify \c none.  The
        option leads to an advisory call to an appropriate operating
        system API where available''',
        choices=['none', 'random', 'sequential']),
    Config('cache_resident', 'false', r'''
        do not ever evict the object's pages from cache. Not compatible with
        LSM tables; see @ref tuning_cache_resident for more information''',
        type='boolean'),
    Config('log', '', r'''
        the transaction log configuration for this object.  Only valid if
        log is enabled in ::wiredtiger_open''',
        type='category', subconfig=[
        Config('enabled', 'true', r'''
            if false, this object has checkpoint-level durability''',
            type='boolean'),
        ]),
    Config('os_cache_max', '0', r'''
        maximum system buffer cache usage, in bytes.  If non-zero, evict
        object blocks from the system buffer cache after that many bytes
        from this object are read or written into the buffer cache''',
        min=0),
    Config('os_cache_dirty_max', '0', r'''
        maximum dirty system buffer cache usage, in bytes.  If non-zero,
        schedule writes for dirty blocks belonging to this object in the
        system buffer cache after that many bytes from this object are
        written into the buffer cache''',
        min=0),
    Config('readonly', 'false', r'''
        the file is read-only. All methods that may modify a file are
        disabled. See @ref readonly for more information''',
        type='boolean'),
    Config('tiered_object', 'false', r'''
        this file is a tiered object. When opened on its own, it is marked as
        readonly and may be restricted in other ways''',
        type='boolean', undoc=True),
]

# Per-file configuration
file_config = format_meta + file_runtime_config + tiered_config + [
    Config('block_allocation', 'best', r'''
        configure block allocation. Permitted values are \c "best" or \c "first";
        the \c "best" configuration uses a best-fit algorithm,
        the \c "first" configuration uses a first-available algorithm during block allocation''',
        choices=['best', 'first',]),
    Config('allocation_size', '4KB', r'''
        the file unit allocation size, in bytes, must a power-of-two;
        smaller values decrease the file space required by overflow
        items, and the default value of 4KB is a good choice absent
        requirements from the operating system or storage device''',
        min='512B', max='128MB'),
    Config('block_compressor', 'none', r'''
        configure a compressor for file blocks.  Permitted values are \c "none"
        or custom compression engine name created with
        WT_CONNECTION::add_compressor.  If WiredTiger has builtin support for
        \c "lz4", \c "snappy", \c "zlib" or \c "zstd" compression, these names
        are also available.  See @ref compression for more information'''),
    Config('checksum', 'on', r'''
        configure block checksums; the permitted values are \c on, \c off, \c uncompressed and
        \c unencrypted. The default is \c on, in which case all block writes include a checksum
        subsequently verified when the block is read. The \c off setting does no checksums,
        the \c uncompressed setting only checksums blocks that are not compressed, and the
        \c unencrypted setting only checksums blocks that are not encrypted.  See @ref
        tune_checksum for more information.''',
        choices=['on', 'off', 'uncompressed', 'unencrypted']),
    Config('dictionary', '0', r'''
        the maximum number of unique values remembered in the Btree
        row-store leaf page value dictionary; see
        @ref file_formats_compression for more information''',
        min='0'),
    Config('encryption', '', r'''
        configure an encryptor for file blocks. When a table is created,
        its encryptor is not implicitly used for any related indices
        or column groups''',
        type='category', subconfig=[
        Config('name', 'none', r'''
            Permitted values are \c "none"
            or custom encryption engine name created with
            WT_CONNECTION::add_encryptor.
            See @ref encryption for more information'''),
        Config('keyid', '', r'''
            An identifier that identifies a unique instance of the encryptor.
            It is stored in clear text, and thus is available when
            the wiredtiger database is reopened.  On the first use
            of a (name, keyid) combination, the WT_ENCRYPTOR::customize
            function is called with the keyid as an argument'''),
        ]),
    Config('format', 'btree', r'''
        the file format''',
        choices=['btree']),
    Config('huffman_key', 'none', r'''
        This option is no longer supported, retained for backward compatibility'''),
    Config('huffman_value', 'none', r'''
        configure Huffman encoding for values.  Permitted values are
        \c "none", \c "english", \c "utf8<file>" or \c "utf16<file>".
        See @ref huffman for more information'''),
    Config('ignore_in_memory_cache_size', 'false', r'''
        allow update and insert operations to proceed even if the cache is
        already at capacity. Only valid in conjunction with in-memory
        databases. Should be used with caution - this configuration allows
        WiredTiger to consume memory over the configured cache limit''',
        type='boolean'),
    Config('internal_key_truncate', 'true', r'''
        configure internal key truncation, discarding unnecessary trailing
        bytes on internal keys (ignored for custom collators)''',
        type='boolean'),
    Config('internal_page_max', '4KB', r'''
        the maximum page size for internal nodes, in bytes; the size
        must be a multiple of the allocation size and is significant
        for applications wanting to avoid excessive L2 cache misses
        while searching the tree.  The page maximum is the bytes of
        uncompressed data, that is, the limit is applied before any
        block compression is done''',
        min='512B', max='512MB'),
    Config('internal_item_max', '0', r'''
        This option is no longer supported, retained for backward compatibility''',
        min=0),
    Config('internal_key_max', '0', r'''
        This option is no longer supported, retained for backward compatibility''',
        min='0'),
    Config('key_gap', '10', r'''
        This option is no longer supported, retained for backward compatibility''',
        min='0'),
    Config('leaf_key_max', '0', r'''
        the largest key stored in a leaf node, in bytes.  If set, keys
        larger than the specified size are stored as overflow items (which
        may require additional I/O to access).  The default value is
        one-tenth the size of a newly split leaf page''',
        min='0'),
    Config('leaf_page_max', '32KB', r'''
        the maximum page size for leaf nodes, in bytes; the size must
        be a multiple of the allocation size, and is significant for
        applications wanting to maximize sequential data transfer from
        a storage device.  The page maximum is the bytes of uncompressed
        data, that is, the limit is applied before any block compression
        is done''',
        min='512B', max='512MB'),
    Config('leaf_value_max', '0', r'''
        the largest value stored in a leaf node, in bytes.  If set, values
        larger than the specified size are stored as overflow items (which
        may require additional I/O to access). If the size is larger than
        the maximum leaf page size, the page size is temporarily ignored
        when large values are written. The default is one-half the size of
        a newly split leaf page''',
        min='0'),
    Config('leaf_item_max', '0', r'''
        This option is no longer supported, retained for backward compatibility''',
        min=0),
    Config('memory_page_image_max', '0', r'''
        the maximum in-memory page image represented by a single storage block.
        Depending on compression efficiency, compression can create storage
        blocks which require significant resources to re-instantiate in the
        cache, penalizing the performance of future point updates. The value
        limits the maximum in-memory page image a storage block will need. If
        set to 0, a default of 4 times \c leaf_page_max is used''',
        min='0'),
    Config('memory_page_max', '5MB', r'''
        the maximum size a page can grow to in memory before being
        reconciled to disk.  The specified size will be adjusted to a lower
        bound of <code>leaf_page_max</code>, and an upper bound of
        <code>cache_size / 10</code>.  This limit is soft - it is possible
        for pages to be temporarily larger than this value.  This setting
        is ignored for LSM trees, see \c chunk_size''',
        min='512B', max='10TB'),
    Config('prefix_compression', 'false', r'''
        configure prefix compression on row-store leaf pages''',
        type='boolean'),
    Config('prefix_compression_min', '4', r'''
        minimum gain before prefix compression will be used on row-store
        leaf pages''',
        min=0),
    Config('split_deepen_min_child', '0', r'''
        minimum entries in a page to consider deepening the tree. Pages
        will be considered for splitting and deepening the search tree
        as soon as there are more than the configured number of children
        ''',
        type='int', undoc=True),
    Config('split_deepen_per_child', '0', r'''
        entries allocated per child when deepening the tree''',
        type='int', undoc=True),
    Config('split_pct', '90', r'''
        the Btree page split size as a percentage of the maximum Btree
        page size, that is, when a Btree page is split, it will be
        split into smaller pages, where each page is the specified
        percentage of the maximum Btree page size''',
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
    Config('version', '(major=0,minor=0)', r'''
        the file version'''),
]

lsm_meta = file_config + lsm_config + [
    Config('last', '0', r'''
        the last allocated chunk ID'''),
    Config('chunks', '', r'''
        active chunks in the LSM tree'''),
    Config('old_chunks', '', r'''
        obsolete chunks in the LSM tree'''),
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
        list of data sources to combine into a tiered storage structure''', type='list'),
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
        comma-separated list of names of column groups.  Each column
        group is stored separately, keyed by the primary key of the
        table.  If no column groups are specified, all columns are
        stored together in a single file.  All value columns in the
        table must appear in at least one column group.  Each column
        group must be created with a separate call to
        WT_SESSION::create''', type='list'),
]

index_only_config = [
    Config('extractor', 'none', r'''
        configure custom extractor for indices.  Permitted values are
        \c "none" or an extractor name created with
        WT_CONNECTION::add_extractor'''),
    Config('immutable', 'false', r'''
        configure the index to be immutable - that is an index is not changed
        by any update to a record in the table''', type='boolean'),
]

colgroup_meta = common_meta + source_meta

index_meta = format_meta + source_meta + index_only_config + [
    Config('index_key_columns', '', r'''
        number of public key columns''', type='int', undoc=True),
]

table_meta = format_meta + table_only_config

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
            the fraction of the block cache that must be full before eviction
            will remove unused blocks''',
            min='30', max='100'),
        Config('size', '0', r'''
            maximum memory to allocate for the block cache''',
            min='0', max='10TB'),
        Config('hashsize', '0', r'''
            number of buckets in the hashtable that keeps track of blocks''',
            min='512', max='256K'),
        Config('max_percent_overhead', '10', r'''
            maximum tolerated overhead expressed as the number of blocks added
            and removed as percent of blocks looked up; cache population
            and eviction will be suppressed if the overhead exceeds the
            supplied threshold''',
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
    Config('cache_size', '100MB', r'''
        maximum heap memory to allocate for the cache. A database should
        configure either \c cache_size or \c shared_cache but not both''',
        min='1MB', max='10TB'),
    Config('cache_max_wait_ms', '0', r'''
        the maximum number of milliseconds an application thread will wait
        for space to be available in cache before giving up. Default will
        wait forever''',
        min=0),
    Config('history_store', '', r'''
        history store configuration options''',
        type='category', subconfig=[
        Config('file_max', '0', r'''
            The maximum number of bytes that WiredTiger is allowed to use for
            its history store mechanism. If the history store file exceeds
            this size, a panic will be triggered. The default value means that
            the history store file is unbounded and may use as much space as
            the filesystem will accommodate. The minimum non-zero setting is
            100MB.''',    # !!! Must match WT_HS_FILE_MIN
            min='0')
        ]),
    Config('cache_overhead', '8', r'''
        assume the heap allocator overhead is the specified percentage, and
        adjust the cache usage by that amount (for example, if there is 10GB
        of data in cache, a percentage of 10 means WiredTiger treats this as
        11GB).  This value is configurable because different heap allocators
        have different overhead and different workloads will have different
        heap allocation sizes and patterns, therefore applications may need to
        adjust this value based on allocator choice and behavior in measured
        workloads''',
        min='0', max='30'),
    Config('checkpoint', '', r'''
        periodically checkpoint the database. Enabling the checkpoint server
        uses a session from the configured session_max''',
        type='category', subconfig=[
        Config('log_size', '0', r'''
            wait for this amount of log record bytes to be written to
                the log between each checkpoint.  If non-zero, this value will
                use a minimum of the log file size.  A database can configure
                both log_size and wait to set an upper bound for checkpoints;
                setting this value above 0 configures periodic checkpoints''',
            min='0', max='2GB'),
        Config('wait', '0', r'''
            seconds to wait between each checkpoint; setting this value
            above 0 configures periodic checkpoints''',
            min='0', max='100000'),
        ]),
    Config('debug_mode', '', r'''
        control the settings of various extended debugging features''',
        type='category', subconfig=[
        Config('corruption_abort', 'true', r'''
            if true, dump the core in the diagnostic mode on encountering the data corruption.''',
            type='boolean'),
        Config('checkpoint_retention', '0', r'''
            adjust log archiving to retain the log records of this number
            of checkpoints. Zero or one means perform normal archiving.''',
            min='0', max='1024'),
        Config('cursor_copy', 'false', r'''
            if true, use the system allocator to make a copy of any data
            returned by a cursor operation and return the copy instead.
            The copy is freed on the next cursor operation. This allows
            memory sanitizers to detect inappropriate references to memory
            owned by cursors.''',
            type='boolean'),
        Config('eviction', 'false', r'''
            if true, modify internal algorithms to change skew to force
            history store eviction to happen more aggressively. This includes but
            is not limited to not skewing newest, not favoring leaf pages,
            and modifying the eviction score mechanism.''',
            type='boolean'),
        Config('log_retention', '0', r'''
            adjust log archiving to retain at least this number of log files, ignored if set to 0.
            (Warning: this option can remove log files required for recovery if no checkpoints
            have yet been done and the number of log files exceeds the configured value. As
            WiredTiger cannot detect the difference between a system that has not yet checkpointed
            and one that will never checkpoint, it might discard log files before any checkpoint is
            done.)''',
            min='0', max='1024'),
        Config('realloc_exact', 'false', r'''
            if true, reallocation of memory will only provide the exact
            amount requested. This will help with spotting memory allocation
            issues more easily.''',
            type='boolean'),
        Config('rollback_error', '0', r'''
            return a WT_ROLLBACK error from a transaction operation about
            every Nth operation to simulate a collision''',
            min='0', max='10M'),
        Config('slow_checkpoint', 'false', r'''
            if true, slow down checkpoint creation by slowing down internal
            page processing.''',
            type='boolean'),
        Config('table_logging', 'false', r'''
            if true, write transaction related information to the log for all
            operations, even operations for tables with logging turned off.
            This setting introduces a log format change that may break older
            versions of WiredTiger. These operations are informational and
            skipped in recovery.''',
            type='boolean'),
        Config('update_restore_evict', 'false', r'''
            if true, control all dirty page evictions through forcing update restore eviction.''',
            type='boolean'),
        ]),
    Config('error_prefix', '', r'''
        prefix string for error messages'''),
    Config('eviction', '', r'''
        eviction configuration options''',
        type='category', subconfig=[
            Config('threads_max', '8', r'''
                maximum number of threads WiredTiger will start to help evict
                pages from cache. The number of threads started will vary
                depending on the current eviction load. Each eviction worker
                thread uses a session from the configured session_max''',
                min=1, max=20),
            Config('threads_min', '1', r'''
                minimum number of threads WiredTiger will start to help evict
                pages from cache. The number of threads currently running will
                vary depending on the current eviction load''',
                min=1, max=20),
            ]),
    Config('eviction_checkpoint_target', '1', r'''
        perform eviction at the beginning of checkpoints to bring the dirty
        content in cache to this level. It is a percentage of the cache size if
        the value is within the range of 0 to 100 or an absolute size when
        greater than 100. The value is not allowed to exceed the \c cache_size.
        Ignored if set to zero.''',
        min=0, max='10TB'),
    Config('eviction_dirty_target', '5', r'''
        perform eviction in worker threads when the cache contains at least
        this much dirty content. It is a percentage of the cache size if the
        value is within the range of 1 to 100 or an absolute size when greater
        than 100. The value is not allowed to exceed the \c cache_size and has
        to be lower than its counterpart \c eviction_dirty_trigger''',
        min=1, max='10TB'),
    Config('eviction_dirty_trigger', '20', r'''
        trigger application threads to perform eviction when the cache contains
        at least this much dirty content. It is a percentage of the cache size
        if the value is within the range of 1 to 100 or an absolute size when
        greater than 100. The value is not allowed to exceed the \c cache_size
        and has to be greater than its counterpart \c eviction_dirty_target.
        This setting only alters behavior if it is lower than eviction_trigger
        ''', min=1, max='10TB'),
    Config('eviction_target', '80', r'''
        perform eviction in worker threads when the cache contains at least
        this much content. It is a percentage of the cache size if the value is
        within the range of 10 to 100 or an absolute size when greater than 100.
        The value is not allowed to exceed the \c cache_size and has to be lower
        than its counterpart \c eviction_trigger''',
        min=10, max='10TB'),
    Config('eviction_trigger', '95', r'''
        trigger application threads to perform eviction when the cache contains
        at least this much content. It is a percentage of the cache size if the
        value is within the range of 10 to 100 or an absolute size when greater
        than 100. The value is not allowed to exceed the \c cache_size and has
        to be greater than its counterpart \c eviction_target''',
        min=10, max='10TB'),
    Config('eviction_updates_target', '0', r'''
        perform eviction in worker threads when the cache contains at least
        this many bytes of updates. It is a percentage of the cache size if the
        value is within the range of 0 to 100 or an absolute size when greater
        than 100. Calculated as half of \c eviction_dirty_target by default.
        The value is not allowed to exceed the \c cache_size and has to be lower
        than its counterpart \c eviction_updates_trigger''',
        min=0, max='10TB'),
    Config('eviction_updates_trigger', '0', r'''
        trigger application threads to perform eviction when the cache contains
        at least this many bytes of updates. It is a percentage of the cache size
        if the value is within the range of 1 to 100 or an absolute size when
        greater than 100\. Calculated as half of \c eviction_dirty_trigger by default.
        The value is not allowed to exceed the \c cache_size and has to be greater than
        its counterpart \c eviction_updates_target. This setting only alters behavior
        if it is lower than \c eviction_trigger''',
        min=0, max='10TB'),
    Config('file_manager', '', r'''
        control how file handles are managed''',
        type='category', subconfig=[
        Config('close_handle_minimum', '250', r'''
            number of handles open before the file manager will look for handles
            to close''', min=0),
        Config('close_idle_time', '30', r'''
            amount of time in seconds a file handle needs to be idle
            before attempting to close it. A setting of 0 means that idle
            handles are not closed''', min=0, max=100000),
        Config('close_scan_interval', '10', r'''
            interval in seconds at which to check for files that are
            inactive and close them''', min=1, max=100000),
        ]),
    Config('io_capacity', '', r'''
        control how many bytes per second are written and read. Exceeding
        the capacity results in throttling.''',
        type='category', subconfig=[
        Config('total', '0', r'''
            number of bytes per second available to all subsystems in total.
            When set, decisions about what subsystems are throttled, and in
            what proportion, are made internally. The minimum non-zero setting
            is 1MB.''',
            min='0', max='1TB'),
        ]),
    Config('json_output', '[]', r'''
        enable JSON formatted messages on the event handler interface. Options are
        given as a list, where each option specifies an event handler category e.g.
        'error' represents the messages from the WT_EVENT_HANDLER::handle_error method.''',
        type='list', choices=[
            'error',
            'message']),
    Config('lsm_manager', '', r'''
        configure database wide options for LSM tree management. The LSM
        manager is started automatically the first time an LSM tree is opened.
        The LSM manager uses a session from the configured session_max''',
        type='category', subconfig=[
        Config('worker_thread_max', '4', r'''
            Configure a set of threads to manage merging LSM trees in
            the database. Each worker thread uses a session handle from
            the configured session_max''',
            min='3',     # !!! Must match WT_LSM_MIN_WORKERS
            max='20'),     # !!! Must match WT_LSM_MAX_WORKERS
        Config('merge', 'true', r'''
            merge LSM chunks where possible''',
            type='boolean')
        ]),
    Config('operation_timeout_ms', '0', r'''
        when non-zero, a requested limit on the number of elapsed real time milliseconds
        application threads will take to complete database operations. Time is measured from the
        start of each WiredTiger API call.  There is no guarantee any operation will not take
        longer than this amount of time. If WiredTiger notices the limit has been exceeded, an
        operation may return a WT_ROLLBACK error. Default is to have no limit''',
        min=1),
    Config('operation_tracking', '', r'''
        enable tracking of performance-critical functions. See
        @ref operation_tracking for more information''',
        type='category', subconfig=[
            Config('enabled', 'false', r'''
                enable operation tracking subsystem''',
                type='boolean'),
            Config('path', '"."', r'''
                the name of a directory into which operation tracking files are
                written. The directory must already exist. If the value is not
                an absolute path, the path is relative to the database home
                (see @ref absolute_path for more information)'''),
        ]),
    Config('shared_cache', '', r'''
        shared cache configuration options. A database should configure
        either a cache_size or a shared_cache not both. Enabling a
        shared cache uses a session from the configured session_max. A
        shared cache can not have absolute values configured for cache
        eviction settings''',
        type='category', subconfig=[
        Config('chunk', '10MB', r'''
            the granularity that a shared cache is redistributed''',
            min='1MB', max='10TB'),
        Config('name', 'none', r'''
            the name of a cache that is shared between databases or
            \c "none" when no shared cache is configured'''),
        Config('quota', '0', r'''
            maximum size of cache this database can be allocated from the
            shared cache. Defaults to the entire shared cache size''',
            type='int'),
        Config('reserve', '0', r'''
            amount of cache this database is guaranteed to have
            available from the shared cache. This setting is per
            database. Defaults to the chunk size''', type='int'),
        Config('size', '500MB', r'''
            maximum memory to allocate for the shared cache. Setting
            this will update the value if one is already set''',
            min='1MB', max='10TB')
        ]),
    Config('statistics', 'none', r'''
        Maintain database statistics, which may impact performance.
        Choosing "all" maintains all statistics regardless of cost,
        "fast" maintains a subset of statistics that are relatively
        inexpensive, "none" turns off all statistics. The "clear"
        configuration resets statistics after they are gathered,
        where appropriate (for example, a cache size statistic is
        not cleared, while the count of cursor insert operations will
        be cleared).   When "clear" is configured for the database,
        gathered statistics are reset each time a statistics cursor
        is used to gather statistics, as well as each time statistics
        are logged using the \c statistics_log configuration.  See
        @ref statistics for more information''',
        type='list',
        choices=['all', 'cache_walk', 'fast', 'none', 'clear', 'tree_walk']),
    Config('tiered_manager', '', r'''
        tiered storage manager configuration options''',
        type='category', undoc=True, subconfig=[
            Config('threads_max', '8', r'''
                maximum number of threads WiredTiger will start to help manage
                tiered storage maintenance. Each worker thread uses a session
                from the configured session_max''',
                min=1, max=20),
            Config('threads_min', '1', r'''
                minimum number of threads WiredTiger will start to help manage
                tiered storage maintenance.''',
                min=1, max=20),
            Config('wait', '0', r'''
                seconds to wait between each periodic housekeeping of
                tiered storage. Setting this value above 0 configures periodic
                management inside WiredTiger''',
                min='0', max='100000'),
            ]),
    Config('timing_stress_for_test', '', r'''
        enable code that interrupts the usual timing of operations with a goal
        of uncovering race conditions and unexpected blocking. This option is
        intended for use with internal stress testing of WiredTiger.''',
        type='list', undoc=True,
        choices=[
        'aggressive_sweep', 'backup_rename', 'checkpoint_reserved_txnid_delay', 'checkpoint_slow',
        'failpoint_history_store_delete_key_from_ts', 'history_store_checkpoint_delay',
        'history_store_search', 'history_store_sweep_race', 'prepare_checkpoint_delay', 'split_1',
        'split_2', 'split_3', 'split_4', 'split_5', 'split_6', 'split_7']),
    Config('verbose', '[]', r'''
        enable messages for various subsystems and operations. Options are given as a list,
        where each message type can optionally define an associated verbosity level, such as
        <code>"verbose=[evictserver,read:1,rts:0]"</code>. Verbosity levels that can be provided
        include <code>0</code> (INFO) and <code>1</code> (DEBUG).''',
        type='list', choices=[
            'api',
            'backup',
            'block',
            'block_cache',
            'checkpoint',
            'checkpoint_cleanup',
            'checkpoint_progress',
            'compact',
            'compact_progress',
            'error_returns',
            'evict',
            'evict_stuck',
            'evictserver',
            'fileops',
            'generation',
            'handleops',
            'history_store',
            'history_store_activity',
            'log',
            'lsm',
            'lsm_manager',
            'metadata',
            'mutex',
            'out_of_order',
            'overflow',
            'read',
            'reconcile',
            'recovery',
            'recovery_progress',
            'rts',
            'salvage',
            'shared_cache',
            'split',
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
        set compatibility version of database.  Changing the compatibility
        version requires that there are no active operations for the duration
        of the call.''',
        type='category', subconfig=
        compatibility_configuration_common)
]
wiredtiger_open_compatibility_configuration = [
    Config('compatibility', '', r'''
        set compatibility version of database.  Changing the compatibility
        version requires that there are no active operations for the duration
        of the call.''',
        type='category', subconfig=
        compatibility_configuration_common + [
        Config('require_max', '', r'''
            required maximum compatibility version of existing data files.
            Must be greater than or equal to any release version set in the
            \c release setting. Has no effect if creating the database.'''),
        Config('require_min', '', r'''
            required minimum compatibility version of existing data files.
            Must be less than or equal to any release version set in the
            \c release setting. Has no effect if creating the database.'''),
    ]),
]

# wiredtiger_open and WT_CONNECTION.reconfigure log configurations.
log_configuration_common = [
    Config('archive', 'true', r'''
        automatically archive unneeded log files''',
        type='boolean'),
    Config('os_cache_dirty_pct', '0', r'''
        maximum dirty system buffer cache usage, as a percentage of the
        log's \c file_max.  If non-zero, schedule writes for dirty blocks
        belonging to the log in the system buffer cache after that percentage
        of the log has been written into the buffer cache without an
        intervening file sync.''',
        min='0', max='100'),
    Config('prealloc', 'true', r'''
        pre-allocate log files''',
        type='boolean'),
    Config('zero_fill', 'false', r'''
        manually write zeroes into log files''',
        type='boolean')
]
connection_reconfigure_log_configuration = [
    Config('log', '', r'''
        enable logging. Enabling logging uses three sessions from the
        configured session_max''',
        type='category', subconfig=
        log_configuration_common)
]
wiredtiger_open_log_configuration = [
    Config('log', '', r'''
        enable logging. Enabling logging uses three sessions from the
        configured session_max''',
        type='category', subconfig=
        log_configuration_common + [
        Config('enabled', 'false', r'''
            enable logging subsystem''',
            type='boolean'),
        Config('compressor', 'none', r'''
            configure a compressor for log records.  Permitted values are
            \c "none" or custom compression engine name created with
            WT_CONNECTION::add_compressor.  If WiredTiger has builtin support
            for \c "lz4", \c "snappy", \c "zlib" or \c "zstd" compression,
            these names are also available. See @ref compression for more
            information'''),
        Config('file_max', '100MB', r'''
            the maximum size of log files''',
            min='100KB',    # !!! Must match WT_LOG_FILE_MIN
            max='2GB'),    # !!! Must match WT_LOG_FILE_MAX
        Config('path', '"."', r'''
            the name of a directory into which log files are written. The
            directory must already exist. If the value is not an absolute path,
            the path is relative to the database home (see @ref absolute_path
            for more information)'''),
        Config('recover', 'on', r'''
            run recovery or error if recovery needs to run after an
            unclean shutdown''',
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
        if non-empty, include statistics for the list of data source
        URIs, if they are open at the time of the statistics logging.
        The list may include URIs matching a single data source
        ("table:mytable"), or a URI matching all data sources of a
        particular type ("table:")''',
        type='list'),
    Config('timestamp', '"%b %d %H:%M:%S"', r'''
        a timestamp prepended to each log record, may contain strftime
        conversion specifications, when \c json is configured, defaults
        to \c "%Y-%m-%dT%H:%M:%S.000Z"'''),
    Config('wait', '0', r'''
        seconds to wait between each write of the log records; setting
        this value above 0 configures statistics logging''',
        min='0', max='100000'),
]
connection_reconfigure_statistics_log_configuration = [
    Config('statistics_log', '', r'''
        log any statistics the database is configured to maintain,
        to a file.  See @ref statistics for more information. Enabling
        the statistics log server uses a session from the configured
        session_max''',
        type='category', subconfig=
        statistics_log_configuration_common)
]

tiered_storage_configuration_common = [
    Config('local_retention', '300', r'''
        time in seconds to retain data on tiered storage on the local tier for
        faster read access''',
        min='0', max='10000'),
    Config('object_target_size', '10M', r'''
        the approximate size of objects before creating them on the
        tiered storage tier''',
        min='100K', max='10TB'),
]
connection_reconfigure_tiered_storage_configuration = [
    Config('tiered_storage', '', r'''
        enable tiered storage. Enabling tiered storage may use one session from the
        configured session_max''',
        type='category', subconfig=
        tiered_storage_configuration_common)
]
wiredtiger_open_tiered_storage_configuration = [
    Config('tiered_storage', '', r'''
        enable tiered storage. Enabling tiered storage may use one session from the
        configured session_max''',
        type='category', undoc=True, subconfig=
        tiered_storage_configuration_common + [
        Config('auth_token', '', r'''
            authentication string identifier'''),
        Config('bucket', '', r'''
            bucket string identifier where the objects should reside'''),
        Config('bucket_prefix', '', r'''
            unique string prefix to identify our objects in the bucket.
            Multiple instances can share the storage bucket and this
            identifier is used in naming objects'''),
        Config('cache_directory', '', r'''
            a directory to store locally cached versions of files in the storage source.  By
            default, it is named with \c "-cache" appended to the bucket name.  A relative
            directory name is relative to the home directory'''),
        Config('name', 'none', r'''
            Permitted values are \c "none"
            or custom storage name created with
            WT_CONNECTION::add_storage_source'''),
    ]),
]

wiredtiger_open_statistics_log_configuration = [
    Config('statistics_log', '', r'''
        log any statistics the database is configured to maintain,
        to a file.  See @ref statistics for more information. Enabling
        the statistics log server uses a session from the configured
        session_max''',
        type='category', subconfig=
        statistics_log_configuration_common + [
        Config('path', '"."', r'''
            the name of a directory into which statistics files are written.
            The directory must already exist. If the value is not an absolute
            path, the path is relative to the database home (see @ref
            absolute_path for more information)''')
        ])
]

session_config = [
    Config('cache_cursors', 'true', r'''
        enable caching of cursors for reuse. Any calls to WT_CURSOR::close
        for a cursor created in this session will mark the cursor
        as cached and keep it available to be reused for later calls
        to WT_SESSION::open_cursor. Cached cursors may be eventually
        closed. This value is inherited from ::wiredtiger_open
        \c cache_cursors''',
        type='boolean'),
    Config('debug', '', r'''
        configure debug specific behavior on a session. Generally only used
        for internal testing purposes.''',
        type='category', subconfig=[
        Config('release_evict_page', 'false', r'''
            Configure the session to evict the page when it is released and
            no longer needed.''',
            type='boolean'),
        ]),
    Config('cache_max_wait_ms', '0', r'''
        the maximum number of milliseconds an application thread will wait
        for space to be available in cache before giving up.
        Default value will be the global setting of the 
        connection config''',
        min=0),
    Config('ignore_cache_size', 'false', r'''
        when set, operations performed by this session ignore the cache size
        and are not blocked when the cache is full.  Note that use of this
        option for operations that create cache pressure can starve ordinary
        sessions that obey the cache size.''',
        type='boolean'),
    Config('isolation', 'snapshot', r'''
        the default isolation level for operations in this session''',
        choices=['read-uncommitted', 'read-committed', 'snapshot']),
]

wiredtiger_open_common =\
    connection_runtime_config +\
    wiredtiger_open_compatibility_configuration +\
    wiredtiger_open_log_configuration +\
    wiredtiger_open_tiered_storage_configuration +\
    wiredtiger_open_statistics_log_configuration + [
    Config('buffer_alignment', '-1', r'''
        in-memory alignment (in bytes) for buffers used for I/O.  The
        default value of -1 indicates a platform-specific alignment value
        should be used (4KB on Linux systems when direct I/O is configured,
        zero elsewhere)''',
        min='-1', max='1MB'),
    Config('builtin_extension_config', '', r'''
        A structure where the keys are the names of builtin extensions and the
        values are passed to WT_CONNECTION::load_extension as the \c config
        parameter (for example,
        <code>builtin_extension_config={zlib={compression_level=3}}</code>)'''),
    Config('cache_cursors', 'true', r'''
        enable caching of cursors for reuse. This is the default value
        for any sessions created, and can be overridden in configuring
        \c cache_cursors in WT_CONNECTION.open_session.''',
        type='boolean'),
    Config('checkpoint_sync', 'true', r'''
        flush files to stable storage when closing or writing
        checkpoints''',
        type='boolean'),
    Config('direct_io', '', r'''
        Use \c O_DIRECT on POSIX systems, and \c FILE_FLAG_NO_BUFFERING on
        Windows to access files.  Options are given as a list, such as
        <code>"direct_io=[data]"</code>.  Configuring \c direct_io requires
        care, see @ref tuning_system_buffer_cache_direct_io for important
        warnings.  Including \c "data" will cause WiredTiger data files to use
        direct I/O, including \c "log" will cause WiredTiger log files to use
        direct I/O, and including \c "checkpoint" will cause WiredTiger data
        files opened at a checkpoint (i.e: read-only) to use direct I/O.
        \c direct_io should be combined with \c write_through to get the
        equivalent of \c O_DIRECT on Windows''',
        type='list', choices=['checkpoint', 'data', 'log']),
    Config('encryption', '', r'''
        configure an encryptor for system wide metadata and logs.
        If a system wide encryptor is set, it is also used for
        encrypting data files and tables, unless encryption configuration
        is explicitly set for them when they are created with
        WT_SESSION::create''',
        type='category', subconfig=[
        Config('name', 'none', r'''
            Permitted values are \c "none"
            or custom encryption engine name created with
            WT_CONNECTION::add_encryptor.
            See @ref encryption for more information'''),
        Config('keyid', '', r'''
            An identifier that identifies a unique instance of the encryptor.
            It is stored in clear text, and thus is available when
            the wiredtiger database is reopened.  On the first use
            of a (name, keyid) combination, the WT_ENCRYPTOR::customize
            function is called with the keyid as an argument'''),
        Config('secretkey', '', r'''
            A string that is passed to the WT_ENCRYPTOR::customize function.
            It is never stored in clear text, so must be given to any
            subsequent ::wiredtiger_open calls to reopen the database.
            It must also be provided to any "wt" commands used with
            this database'''),
        ]),
    Config('extensions', '', r'''
        list of shared library extensions to load (using dlopen).
        Any values specified to a library extension are passed to
        WT_CONNECTION::load_extension as the \c config parameter
        (for example,
        <code>extensions=(/path/ext.so={entry=my_entry})</code>)''',
        type='list'),
    Config('file_extend', '', r'''
        file extension configuration.  If set, extend files of the set
        type in allocations of the set size, instead of a block at a
        time as each new block is written.  For example,
        <code>file_extend=(data=16MB)</code>. If set to 0, disable the file
        extension for the set type. For log files, the allowed range is
        between 100KB and 2GB; values larger than the configured maximum log
        size and the default config would extend log files in allocations of
        the maximum log file size.''',
        type='list', choices=['data', 'log']),
    Config('file_close_sync', 'true', r'''
        control whether to flush modified files to storage independent
        of a global checkpoint when closing file handles to acquire exclusive
        access to a table. If set to false, and logging is disabled, API calls that
        require exclusive access to tables will return EBUSY if there have been
        changes made to the table since the last global checkpoint. When logging
        is enabled, the value for <code>file_close_sync</code> has no effect, and,
        modified file is always flushed to storage when closing file handles to
        acquire exclusive access to the table''',
        type='boolean'),
    Config('hash', '', r'''
        manage resources around hash bucket arrays. All values must be a power of two.
        Note that setting large values can significantly increase memory usage inside
        WiredTiger''',
        type='category', subconfig=[
        Config('buckets', 512, r'''
            configure the number of hash buckets for most system hash arrays''',
            min='64', max='65536'),
        Config('dhandle_buckets', 512, r'''
            configure the number of hash buckets for hash arrays relating to data handles''',
            min='64', max='65536'),
        ]),
    Config('hazard_max', '1000', r'''
        maximum number of simultaneous hazard pointers per session
        handle''',
        min=15, undoc=True),
    Config('mmap', 'true', r'''
        Use memory mapping when accessing files in a read-only mode''',
        type='boolean'),
    Config('mmap_all', 'false', r'''
        Use memory mapping to read and write all data files, may not be configured with direct
        I/O''',
        type='boolean'),
    Config('multiprocess', 'false', r'''
        permit sharing between processes (will automatically start an
        RPC server for primary processes and use RPC for secondary
        processes). <b>Not yet supported in WiredTiger</b>''',
        type='boolean'),
    Config('readonly', 'false', r'''
        open connection in read-only mode.  The database must exist.  All
        methods that may modify a database are disabled.  See @ref readonly
        for more information''',
        type='boolean'),
    Config('salvage', 'false', r'''
        open connection and salvage any WiredTiger-owned database and log
        files that it detects as corrupted. This API should only be used
        after getting an error return of WT_TRY_SALVAGE.
        Salvage rebuilds files in place, overwriting existing files.
        We recommend making a backup copy of all files with the
        WiredTiger prefix prior to passing this flag.''',
        type='boolean'),
    Config('session_max', '100', r'''
        maximum expected number of sessions (including server
        threads)''',
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
            whether to sync the log on every commit by default, can be
            overridden by the \c sync setting to
            WT_SESSION::commit_transaction''',
            type='boolean'),
        Config('method', 'fsync', r'''
            the method used to ensure log records are stable on disk, see
            @ref tune_durability for more information''',
            choices=['dsync', 'fsync', 'none']),
        ]),
    Config('verify_metadata', 'false', r'''
        open connection and verify any WiredTiger metadata. This API
        allows verification and detection of corruption in WiredTiger metadata.''',
        type='boolean'),
    Config('write_through', '', r'''
        Use \c FILE_FLAG_WRITE_THROUGH on Windows to write to files.  Ignored
        on non-Windows systems.  Options are given as a list, such as
        <code>"write_through=[data]"</code>.  Configuring \c write_through
        requires care, see @ref tuning_system_buffer_cache_direct_io for
        important warnings.  Including \c "data" will cause WiredTiger data
        files to write through cache, including \c "log" will cause WiredTiger
        log files to write through cache. \c write_through should be combined
        with \c direct_io to get the equivalent of POSIX \c O_DIRECT on
        Windows''',
        type='list', choices=['data', 'log']),
]

wiredtiger_open = wiredtiger_open_common + [
   Config('config_base', 'true', r'''
        write the base configuration file if creating the database.  If
        \c false in the config passed directly to ::wiredtiger_open, will
        ignore any existing base configuration file in addition to not creating
        one.  See @ref config_base for more information''',
        type='boolean'),
    Config('create', 'false', r'''
        create the database if it does not exist''',
        type='boolean'),
    Config('exclusive', 'false', r'''
        fail if the database already exists, generally used with the
        \c create option''',
        type='boolean'),
    Config('in_memory', 'false', r'''
        keep data in-memory only. See @ref in_memory for more information''',
        type='boolean'),
    Config('use_environment', 'true', r'''
        use the \c WIREDTIGER_CONFIG and \c WIREDTIGER_HOME environment
        variables if the process is not running with special privileges.
        See @ref home for more information''',
        type='boolean'),
    Config('use_environment_priv', 'false', r'''
        use the \c WIREDTIGER_CONFIG and \c WIREDTIGER_HOME environment
        variables even if the process is running with special privileges.
        See @ref home for more information''',
        type='boolean'),
]

cursor_runtime_config = [
    Config('append', 'false', r'''
        append the value as a new record, creating a new record
        number key; valid only for cursors with record number keys''',
        type='boolean'),
    Config('overwrite', 'true', r'''
        configures whether the cursor's insert, update and remove
        methods check the existing state of the record.  If \c overwrite
        is \c false, WT_CURSOR::insert fails with ::WT_DUPLICATE_KEY
        if the record exists, WT_CURSOR::update fails with ::WT_NOTFOUND
        if the record does not exist''',
        type='boolean'),
    Config('prefix_search', 'false', r'''
        when performing a search near for a prefix, if set to true this
        configuration will allow the search near to exit early if it has left
        the key range defined by the prefix. This is relevant when the table
        contains a large number of records which potentially aren't visible to
        the caller of search near, as such a large number of records could be skipped.
        The prefix_search configuration provides a fast exit in this scenario.''', type='boolean',
        undoc=True),
]

methods = {
'colgroup.meta' : Method(colgroup_meta),

'file.config' : Method(file_config),

'file.meta' : Method(file_meta),

'index.meta' : Method(index_meta),

'lsm.meta' : Method(lsm_meta),

'object.meta' : Method(object_meta),

'table.meta' : Method(table_meta),

'tier.meta' : Method(tier_meta),

'tiered.meta' : Method(tiered_meta),

'WT_CURSOR.close' : Method([]),

'WT_CURSOR.reconfigure' : Method(cursor_runtime_config),

'WT_SESSION.alter' : Method(file_runtime_config + [
    Config('checkpoint', '', r'''
        the file checkpoint entries''', undoc=True),
    Config('exclusive_refreshed', 'true', r'''
        refresh the in memory state and flush the metadata change to disk,
        disabling this flag is dangerous - it will only re-write the
        metadata without refreshing the in-memory information or creating
        a checkpoint. The update will also only be applied to table URI
        entries in the metadata, not their sub-entries.''',
        type='boolean', undoc=True),
]),

'WT_SESSION.close' : Method([]),

'WT_SESSION.compact' : Method([
    Config('timeout', '1200', r'''
        maximum amount of time to allow for compact in seconds. The
        actual amount of time spent in compact may exceed the configured
        value. A value of zero disables the timeout''',
        type='int'),
]),

'WT_SESSION.create' : Method(file_config + lsm_config + tiered_config +
        source_meta + index_only_config + table_only_config + [
    Config('exclusive', 'false', r'''
        fail if the object exists.  When false (the default), if the
        object exists, check that its settings match the specified
        configuration''',
        type='boolean'),
    Config('import', '', r'''
        configure import of an existing object into the currently running database''',
        type='category', subconfig=[
        Config('enabled', 'false', r'''
            whether to import the input URI from disk''',
            type='boolean'),
        Config('repair', 'false', r'''
            whether to reconstruct the metadata from the raw file content''',
            type='boolean'),
        Config('file_metadata', '', r'''
            the file configuration extracted from the metadata of the export database'''),
        ]),
]),

'WT_SESSION.drop' : Method([
    Config('checkpoint_wait', 'true', r'''
        wait for concurrent checkpoints to complete before attempting the drop
        operation. If \c checkpoint_wait=false, attempt the drop operation
        without waiting, returning EBUSY if the operation conflicts with a
        running checkpoint''',
        type='boolean', undoc=True),
    Config('force', 'false', r'''
        return success if the object does not exist''',
        type='boolean'),
    Config('lock_wait', 'true', r'''
        wait for locks, if \c lock_wait=false, fail if any required locks are
        not available immediately''',
        type='boolean', undoc=True),
    Config('remove_files', 'true', r'''
        if the underlying files should be removed''',
        type='boolean'),
]),

'WT_SESSION.join' : Method([
    Config('compare', '"eq"', r'''
        modifies the set of items to be returned so that the index key
        satisfies the given comparison relative to the key set in this
        cursor''',
        choices=['eq', 'ge', 'gt', 'le', 'lt']),
    Config('count', '', r'''
        set an approximate count of the elements that would be included in
        the join.  This is used in sizing the bloom filter, and also influences
        evaluation order for cursors in the join. When the count is equal
        for multiple bloom filters in a composition of joins, the bloom
        filter may be shared''',
        type='int'),
    Config('bloom_bit_count', '16', r'''
        the number of bits used per item for the bloom filter''',
        min='2', max='1000'),
    Config('bloom_false_positives', 'false', r'''
        return all values that pass the bloom filter, without eliminating
        any false positives''',
        type='boolean'),
    Config('bloom_hash_count', '8', r'''
        the number of hash values per item for the bloom filter''',
        min='2', max='100'),
    Config('operation', '"and"', r'''
        the operation applied between this and other joined cursors.
        When "operation=and" is specified, all the conditions implied by
        joins must be satisfied for an entry to be returned by the join cursor;
        when "operation=or" is specified, only one must be satisfied.
        All cursors joined to a join cursor must have matching operations''',
        choices=['and', 'or']),
    Config('strategy', '', r'''
        when set to bloom, a bloom filter is created and populated for
        this index. This has an up front cost but may reduce the number
        of accesses to the main table when iterating the joined cursor.
        The bloom setting requires that count be set''',
        choices=['bloom', 'default']),
]),

'WT_SESSION.log_flush' : Method([
    Config('sync', 'on', r'''
        forcibly flush the log and wait for it to achieve the synchronization
        level specified.  The \c off setting forces any
        buffered log records to be written to the file system.  The
        \c on setting forces log records to be written to the storage device''',
        choices=['off', 'on']),
]),

'WT_SESSION.log_printf' : Method([]),

'WT_SESSION.open_cursor' : Method(cursor_runtime_config + [
    Config('bulk', 'false', r'''
        configure the cursor for bulk-loading, a fast, initial load path
        (see @ref tune_bulk_load for more information).  Bulk-load may
        only be used for newly created objects and applications should
        use the WT_CURSOR::insert method to insert rows.  When
        bulk-loading, rows must be loaded in sorted order.  The value
        is usually a true/false flag; when bulk-loading fixed-length
        column store objects, the special value \c bitmap allows chunks
        of a memory resident bitmap to be loaded directly into a file
        by passing a \c WT_ITEM to WT_CURSOR::set_value where the \c
        size field indicates the number of records in the bitmap (as
        specified by the object's \c value_format configuration).
        Bulk-loaded bitmap values must end on a byte boundary relative
        to the bit count (except for the last set of values loaded)'''),
    Config('checkpoint', '', r'''
        the name of a checkpoint to open (the reserved name
        "WiredTigerCheckpoint" opens the most recent internal
        checkpoint taken for the object).  The cursor does not
        support data modification'''),
    Config('checkpoint_wait', 'true', r'''
        wait for the checkpoint lock, if \c checkpoint_wait=false, open the
        cursor without taking a lock, returning EBUSY if the operation
        conflicts with a running checkpoint''',
        type='boolean', undoc=True),
    Config('debug', '', r'''
        configure debug specific behavior on a cursor. Generally only
        used for internal testing purposes''',
        type='category', subconfig=[
        Config('release_evict', 'false', r'''
            Configure the cursor to evict the page positioned on when the
            reset API is used''',
            type='boolean')
        ]),
    Config('dump', '', r'''
        configure the cursor for dump format inputs and outputs: "hex"
        selects a simple hexadecimal format, "json" selects a JSON format
        with each record formatted as fields named by column names if
        available, "pretty" selects a human-readable format (making it
        incompatible with the "load"), "pretty_hex" is similar to "pretty" (also incompatible with
        "load") except raw byte data elements will be printed like "hex" format, and
        "print" selects a format where only non-printing characters are hexadecimal encoded. These
        formats are compatible with the @ref util_dump and @ref util_load commands''',
        choices=['hex', 'json', 'pretty', 'pretty_hex', 'print']),
    Config('incremental', '', r'''
        configure the cursor for block incremental backup usage. These formats
        are only compatible with the backup data source; see @ref backup''',
        type='category', subconfig=[
        Config('consolidate', 'false', r'''
            causes block incremental backup information to be consolidated if adjacent
            granularity blocks are modified. If false, information will be returned in
            granularity sized blocks only. This must be set on the primary backup cursor and it
            applies to all files for this backup''',
            type='boolean'),
        Config('enabled', 'false', r'''
            whether to configure this backup as the starting point for a subsequent
            incremental backup''',
            type='boolean'),
        Config('file', '', r'''
            the file name when opening a duplicate incremental backup cursor.
            That duplicate cursor will return the block modifications relevant
            to the given file name'''),
        Config('force_stop', 'false', r'''
            causes all block incremental backup information to be released. This is
            on an open_cursor call and the resources will be released when this
            cursor is closed. No other operations should be done on this open cursor''',
            type='boolean'),
        Config('granularity', '16MB', r'''
            this setting manages the granularity of how WiredTiger maintains modification
            maps internally. The larger the granularity, the smaller amount of information
            WiredTiger need to maintain''',
            min='4KB', max='2GB'),
        Config('src_id', '', r'''
            a string that identifies a previous checkpoint backup source as the source
            of this incremental backup. This identifier must have already been created
            by use of the 'this_id' configuration in an earlier backup. A source id is
            required to begin an incremental backup'''),
        Config('this_id', '', r'''
            a string that identifies the current system state  as a future backup source
            for an incremental backup via 'src_id'. This identifier is required when opening
            an incremental backup cursor and an error will be returned if one is not provided'''),
        ]),
    Config('next_random', 'false', r'''
        configure the cursor to return a pseudo-random record from the
        object when the WT_CURSOR::next method is called; valid only for
        row-store cursors. See @ref cursor_random for details''',
        type='boolean'),
    Config('next_random_sample_size', '0', r'''
        cursors configured by \c next_random to return pseudo-random
        records from the object randomly select from the entire object,
        by default. Setting \c next_random_sample_size to a non-zero
        value sets the number of samples the application expects to take
        using the \c next_random cursor. A cursor configured with both
        \c next_random and \c next_random_sample_size attempts to divide
        the object into \c next_random_sample_size equal-sized pieces,
        and each retrieval returns a record from one of those pieces. See
        @ref cursor_random for details'''),
    Config('raw', 'false', r'''
        ignore the encodings for the key and value, manage data as if
        the formats were \c "u".  See @ref cursor_raw for details''',
        type='boolean'),
    Config('read_once', 'false', r'''
        results that are brought into cache from disk by this cursor will be
        given less priority in the cache.''',
        type='boolean'),
    Config('readonly', 'false', r'''
        only query operations are supported by this cursor. An error is
        returned if a modification is attempted using the cursor.  The
        default is false for all cursor types except for metadata
        cursors''',
        type='boolean'),
    Config('skip_sort_check', 'false', r'''
        skip the check of the sort order of each bulk-loaded key''',
        type='boolean', undoc=True),
    Config('statistics', '', r'''
        Specify the statistics to be gathered.  Choosing "all" gathers
        statistics regardless of cost and may include traversing on-disk files;
        "fast" gathers a subset of relatively inexpensive statistics.  The
        selection must agree with the database \c statistics configuration
        specified to ::wiredtiger_open or WT_CONNECTION::reconfigure.  For
        example, "all" or "fast" can be configured when the database is
        configured with "all", but the cursor open will fail if "all" is
        specified when the database is configured with "fast", and the cursor
        open will fail in all cases when the database is configured with
        "none".  If "size" is configured, only the underlying size of the
        object on disk is filled in and the object is not opened.  If \c
        statistics is not configured, the default configuration is the database
        configuration.  The "clear" configuration resets statistics after
        gathering them, where appropriate (for example, a cache size statistic
        is not cleared, while the count of cursor insert operations will be
        cleared).  See @ref statistics for more information''',
        type='list',
        choices=['all', 'cache_walk', 'fast', 'clear', 'size', 'tree_walk']),
    Config('target', '', r'''
        if non-empty, backup the list of objects; valid only for a
        backup data source''',
        type='list'),
]),

'WT_SESSION.query_timestamp' : Method([
    Config('get', 'read', r'''
        specify which timestamp to query: \c commit returns the most recently
        set commit_timestamp.  \c first_commit returns the first set
        commit_timestamp.  \c prepare returns the timestamp used in preparing a
        transaction.  \c read returns the timestamp at which the transaction is
        reading at.  See @ref transaction_timestamps''',
        choices=['commit', 'first_commit', 'prepare', 'read']),
]),

'WT_SESSION.reset_snapshot' : Method([]),
'WT_SESSION.rename' : Method([]),
'WT_SESSION.reset' : Method([]),
'WT_SESSION.salvage' : Method([
    Config('force', 'false', r'''
        force salvage even of files that do not appear to be WiredTiger
        files''',
        type='boolean'),
]),

'WT_SESSION.flush_tier' : Method([
    Config('flush_timestamp', '', r'''
        flush objects to all storage sources using the specified timestamp.
        The supplied value must not be older than the current oldest timestamp and it must
        not be newer than the stable timestamp'''),
    Config('force', 'false', r'''
        force sharing of all data''',
        type='boolean'),
    Config('lock_wait', 'true', r'''
        wait for locks, if \c lock_wait=false, fail if any required locks are
        not available immediately''',
        type='boolean'),
    Config('sync', 'on', r'''
        wait for all objects to be flushed to the shared storage to the level
        specified. The \c off setting does not wait for any
        objects to be written to the tiered storage system but returns immediately after
        generating the objects and work units for an internal thread.  The
        \c on setting causes the caller to wait until all work queued for this call to
        be completely processed before returning''',
        choices=['off', 'on']),
    Config('timeout', '0', r'''
        maximum amount of time to allow for waiting for previous flushing
        of objects, in seconds. The actual amount of time spent waiting may
        exceed the configured value. A value of zero disables the timeout''',
        type='int'),
]),

'WT_SESSION.strerror' : Method([]),

'WT_SESSION.truncate' : Method([]),
'WT_SESSION.upgrade' : Method([]),
'WT_SESSION.verify' : Method([
    Config('dump_address', 'false', r'''
        Display page addresses, time windows, and page types as
        pages are verified, using the application's message handler,
        intended for debugging''',
        type='boolean'),
    Config('dump_blocks', 'false', r'''
        Display the contents of on-disk blocks as they are verified,
        using the application's message handler, intended for debugging''',
        type='boolean'),
    Config('dump_layout', 'false', r'''
        Display the layout of the files as they are verified, using the
        application's message handler, intended for debugging; requires
        optional support from the block manager''',
        type='boolean'),
    Config('dump_offsets', '', r'''
        Display the contents of specific on-disk blocks,
        using the application's message handler, intended for debugging''',
        type='list'),
    Config('dump_pages', 'false', r'''
        Display the contents of in-memory pages as they are verified,
        using the application's message handler, intended for debugging''',
        type='boolean'),
    Config('stable_timestamp', 'false', r'''
        Ensure that no data has a start timestamp after the stable timestamp,
        to be run after rollback_to_stable.''',
        type='boolean'),
    Config('strict', 'false', r'''
        Treat any verification problem as an error; by default, verify will
        warn, but not fail, in the case of errors that won't affect future
        behavior (for example, a leaked block)''',
        type='boolean'),
]),

'WT_SESSION.begin_transaction' : Method([
    Config('ignore_prepare', 'false', r'''
        whether to ignore the updates by other prepared transactions as part of
        read operations of this transaction.  When \c true, forces the
        transaction to be read-only.  Use \c force to ignore prepared updates
        and permit writes (which can cause lost updates unless the application
        knows something about the relationship between prepared transactions
        and the updates that are ignoring them)''',
        choices=['false', 'force', 'true']),
    Config('isolation', '', r'''
        the isolation level for this transaction; defaults to the
        session's isolation level''',
        choices=['read-uncommitted', 'read-committed', 'snapshot']),
    Config('name', '', r'''
        name of the transaction for tracing and debugging'''),
    Config('operation_timeout_ms', '0', r'''
        when non-zero, a requested limit on the time taken to complete operations in this
        transaction. Time is measured in real time milliseconds from the start of each WiredTiger
        API call. There is no guarantee any operation will not take longer than this amount of time.
        If WiredTiger notices the limit has been exceeded, an operation may return a WT_ROLLBACK
        error. Default is to have no limit''',
        min=1),
    Config('priority', 0, r'''
        priority of the transaction for resolving conflicts.
        Transactions with higher values are less likely to abort''',
        min='-100', max='100'),
    Config('read_before_oldest', 'false', r'''
        allows the caller to specify a read timestamp less than the oldest timestamp but newer
        than or equal to the pinned timestamp. Cannot be set to true while also rounding up the read
        timestamp. See @ref transaction_timestamps''', type='boolean'),
    Config('read_timestamp', '', r'''
        read using the specified timestamp.  The supplied value must not be
        older than the current oldest timestamp.  See
        @ref transaction_timestamps'''),
    Config('roundup_timestamps', '', r'''
        round up timestamps of the transaction. This setting alters the
        visibility expected in a transaction. See @ref
        transaction_timestamps''',
        type='category', subconfig= [
        Config('prepared', 'false', r'''
            applicable only for prepared transactions. Indicates if the prepare
            timestamp and the commit timestamp of this transaction can be
            rounded up. If the prepare timestamp is less than the oldest
            timestamp, the prepare timestamp  will be rounded to the oldest
            timestamp. If the commit timestamp is less than the prepare
            timestamp, the commit timestamp will be rounded up to the prepare
            timestamp''', type='boolean'),
        Config('read', 'false', r'''
            if the read timestamp is less than the oldest timestamp, the
            read timestamp will be rounded up to the oldest timestamp''',
            type='boolean'),
        ]),
    Config('sync', '', r'''
        whether to sync log records when the transaction commits,
        inherited from ::wiredtiger_open \c transaction_sync''',
        type='boolean')
]),

'WT_SESSION.commit_transaction' : Method([
    Config('commit_timestamp', '', r'''
        set the commit timestamp for the current transaction.  The supplied
        value must not be older than the first commit timestamp set for the
        current transaction.  The value must also not be older than the
        current oldest and stable timestamps.  See
        @ref transaction_timestamps'''),
    Config('durable_timestamp', '', r'''
        set the durable timestamp for the current transaction.  The supplied
        value must not be older than the commit timestamp set for the
        current transaction.  The value must also not be older than the
        current stable timestamp.  See
        @ref transaction_timestamps'''),
    Config('operation_timeout_ms', '0', r'''
        when non-zero, a requested limit on the time taken to complete operations in this
        transaction. Time is measured in real time milliseconds from the start of each WiredTiger
        API call. There is no guarantee any operation will not take longer than this amount of time.
        If WiredTiger notices the limit has been exceeded, an operation may return a WT_ROLLBACK
        error. Default is to have no limit''',
        min=1),
    Config('sync', '', r'''
        override whether to sync log records when the transaction commits,
        inherited from ::wiredtiger_open \c transaction_sync.  The \c off setting does not
        wait for record to be written or synchronized.  The
        \c on setting forces log records to be written to the storage device''',
        choices=['off', 'on']),
]),

'WT_SESSION.prepare_transaction' : Method([
    Config('prepare_timestamp', '', r'''
        set the prepare timestamp for the updates of the current transaction.
        The supplied value must not be older than any active read timestamps.
        See @ref transaction_timestamps'''),
]),

'WT_SESSION.timestamp_transaction' : Method([
    Config('commit_timestamp', '', r'''
        set the commit timestamp for the current transaction.  The supplied
        value must not be older than the first commit timestamp set for the
        current transaction.  The value must also not be older than the
        current oldest and stable timestamps.  See
        @ref transaction_timestamps'''),
    Config('durable_timestamp', '', r'''
        set the durable timestamp for the current transaction.  The supplied
        value must not be older than the commit timestamp set for the
        current transaction.  The value must also not be older than the
        current stable timestamp.  See
        @ref transaction_timestamps'''),
    Config('prepare_timestamp', '', r'''
        set the prepare timestamp for the updates of the current transaction.
        The supplied value must not be older than any active read timestamps.
        See @ref transaction_timestamps'''),
    Config('read_timestamp', '', r'''
        read using the specified timestamp.  The supplied value must not be
        older than the current oldest timestamp.  This can only be set once
        for a transaction. See @ref transaction_timestamps'''),
]),

'WT_SESSION.rollback_transaction' : Method([
    Config('operation_timeout_ms', '0', r'''
        when non-zero, a requested limit on the time taken to complete operations in this
        transaction. Time is measured in real time milliseconds from the start of each WiredTiger
        API call. There is no guarantee any operation will not take longer than this amount of time.
        If WiredTiger notices the limit has been exceeded, an operation may return a WT_ROLLBACK
        error. Default is to have no limit''',
        min=1),
]),

'WT_SESSION.checkpoint' : Method([
    Config('drop', '', r'''
        specify a list of checkpoints to drop.
        The list may additionally contain one of the following keys:
        \c "from=all" to drop all checkpoints,
        \c "from=<checkpoint>" to drop all checkpoints after and
        including the named checkpoint, or
        \c "to=<checkpoint>" to drop all checkpoints before and
        including the named checkpoint.  Checkpoints cannot be
        dropped if open in a cursor.  While a hot backup is in
        progress, checkpoints created prior to the start of the
        backup cannot be dropped''', type='list'),
    Config('force', 'false', r'''
        if false (the default), checkpoints may be skipped if the underlying object has not been
        modified, if true, this option forces the checkpoint''',
        type='boolean'),
    Config('name', '', r'''
        if set, specify a name for the checkpoint (note that checkpoints
        including LSM trees may not be named)'''),
    Config('target', '', r'''
        if non-empty, checkpoint the list of objects''', type='list'),
    Config('use_timestamp', 'true', r'''
        if true (the default), create the checkpoint as of the last stable timestamp if timestamps
        are in use, or all current updates if there is no stable timestamp set. If false, this
        option generates a checkpoint with all updates including those later than the timestamp''',
        type='boolean'),
]),

'WT_CONNECTION.add_collator' : Method([]),
'WT_CONNECTION.add_compressor' : Method([]),
'WT_CONNECTION.add_data_source' : Method([]),
'WT_CONNECTION.add_encryptor' : Method([]),
'WT_CONNECTION.add_extractor' : Method([]),
'WT_CONNECTION.add_storage_source' : Method([]),
'WT_CONNECTION.close' : Method([
    Config('leak_memory', 'false', r'''
        don't free memory during close''',
        type='boolean'),
    Config('use_timestamp', 'true', r'''
        by default, create the close checkpoint as of the last stable timestamp
        if timestamps are in use, or all current updates if there is no
        stable timestamp set.  If false, this option generates a checkpoint
        with all updates''',
        type='boolean'),
]),
'WT_CONNECTION.debug_info' : Method([
    Config('cache', 'false', r'''
        print cache information''', type='boolean'),
    Config('cursors', 'false', r'''
        print all open cursor information''', type='boolean'),
    Config('handles', 'false', r'''
        print open handles information''', type='boolean'),
    Config('log', 'false', r'''
        print log information''', type='boolean'),
    Config('sessions', 'false', r'''
        print open session information''', type='boolean'),
    Config('txn', 'false', r'''
        print global txn information''', type='boolean'),
]),
'WT_CONNECTION.reconfigure' : Method(
    connection_reconfigure_compatibility_configuration +\
    connection_reconfigure_log_configuration +\
    connection_reconfigure_statistics_log_configuration +\
    connection_reconfigure_tiered_storage_configuration +\
    connection_runtime_config
),
'WT_CONNECTION.set_file_system' : Method([]),

'WT_CONNECTION.load_extension' : Method([
    Config('config', '', r'''
        configuration string passed to the entry point of the
        extension as its WT_CONFIG_ARG argument'''),
    Config('early_load', 'false', r'''
        whether this extension should be loaded at the beginning of
        ::wiredtiger_open. Only applicable to extensions loaded via the
        wiredtiger_open configurations string''',
        type='boolean'),
    Config('entry', 'wiredtiger_extension_init', r'''
        the entry point of the extension, called to initialize the
        extension when it is loaded.  The signature of the function
        must match ::wiredtiger_extension_init'''),
    Config('terminate', 'wiredtiger_extension_terminate', r'''
        an optional function in the extension that is called before
        the extension is unloaded during WT_CONNECTION::close.  The
        signature of the function must match
        ::wiredtiger_extension_terminate'''),
]),

'WT_CONNECTION.open_session' : Method(session_config),

'WT_CONNECTION.query_timestamp' : Method([
    Config('get', 'all_durable', r'''
        specify which timestamp to query:
        \c all_durable returns the largest timestamp such that all timestamps
        up to that value have been made durable, \c last_checkpoint returns the
        timestamp of the most recent stable checkpoint, \c oldest returns the
        most recent \c oldest_timestamp set with WT_CONNECTION::set_timestamp,
        \c oldest_reader returns the minimum of the read timestamps of all
        active readers \c pinned returns the minimum of the \c oldest_timestamp
        and the read timestamps of all active readers, \c recovery returns the
        timestamp of the most recent stable checkpoint taken prior to a shutdown
        and \c stable returns the most recent \c stable_timestamp set with
        WT_CONNECTION::set_timestamp. See @ref transaction_timestamps''',
        choices=['all_durable','last_checkpoint',
            'oldest','oldest_reader','pinned','recovery','stable']),
]),

'WT_CONNECTION.set_timestamp' : Method([
    Config('commit_timestamp', '', r'''
        (deprecated) reset the maximum commit timestamp tracked by WiredTiger.
        This will cause future calls to WT_CONNECTION::query_timestamp to
        ignore commit timestamps greater than the specified value until the
        next commit moves the tracked commit timestamp forwards.  This is only
        intended for use where the application is rolling back locally
        committed transactions. The supplied value must not be older than the
        current oldest and stable timestamps.
        See @ref transaction_timestamps'''),
    Config('durable_timestamp', '', r'''
        reset the maximum durable timestamp tracked by WiredTiger.  This will
        cause future calls to WT_CONNECTION::query_timestamp to ignore durable
        timestamps greater than the specified value until the next durable
        timestamp moves the tracked durable timestamp forwards.  This is only
        intended for use where the application is rolling back locally committed
        transactions. The supplied value must not be older than the current
        oldest and stable timestamps.  See @ref transaction_timestamps'''),
    Config('force', 'false', r'''
        set timestamps even if they violate normal ordering requirements.
        For example allow the \c oldest_timestamp to move backwards''',
        type='boolean'),
    Config('oldest_timestamp', '', r'''
        future commits and queries will be no earlier than the specified
        timestamp.  Supplied values must be monotonically increasing, any
        attempt to set the value to older than the current is silently ignored.
        The supplied value must not be newer than the current
        stable timestamp.  See @ref transaction_timestamps'''),
    Config('stable_timestamp', '', r'''
        checkpoints will not include commits that are newer than the specified
        timestamp in tables configured with \c log=(enabled=false).  Supplied
        values must be monotonically increasing, any attempt to set the value to
        older than the current is silently ignored.  The supplied value must
        not be older than the current oldest timestamp.  See
        @ref transaction_timestamps'''),
]),

'WT_CONNECTION.rollback_to_stable' : Method([]),

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
