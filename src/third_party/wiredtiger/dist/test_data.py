# This file is a python script that describes the cpp test framework test configuration options.

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

key_config=[
    Config('key_size', 0, r'''
        The size of the keys to be created''', min=0, max=10000),
]

value_config = [
    Config('value_size', 0, r'''
        The size of the values to be created''', min=0, max=1000000000),
]

scale_config = [
    Config('collection_count', 1, r'''
        The number of colections the workload generator operates over''', min=0, max=200000),
    Config('key_count', 0, r'''
        The number of keys to be operated on per colection''', min=0, max=1000000),
]

throttle_config = [
    Config('rate_per_second',1,r'''
        The number of times an operation should be performed per second''', min=1,max=1000),
]

stat_config = [
    Config('enabled', 'false', r'''
        Whether or not this statistic is relevant to the workload''',
        type='boolean'),
]

limit_stat = stat_config + [
    Config('limit', 0, r'''
    The limit value a statistic is allowed to reach''')
]

load_config = key_config + value_config + scale_config

workload_config = [
    Config('enable_tracking', 'true', r'''
        Enables tracking to perform validation''', type='boolean'),
    Config('duration_seconds', 0, r'''
        The duration that the workload run phase will last''', min=0, max=1000000),
    Config('read_threads', 0, r'''
        The number of threads performing read operations''', min=0, max=100),
    Config('insert_threads', 0, r'''
        The number of threads performing insert operations''', min=0, max=20),
    Config('insert_config',0, r'''
        The definition of the record being inserted''',
        subconfig=load_config),
    Config('update_threads', 0, r'''
        The number of threads performing update operations''', min=0, max=20),
    Config('update_config',0,r''',
        The definition of the record being updated''', subconfig=load_config)
]

test_config = [
    Config('cache_size_mb', 0, r'''
        The cache size that wiredtiger will be configured to run with''', min=0, max=100000000000)
]

runtime_monitor_config = throttle_config +[
    Config('stat_cache_size', '', '''
        The maximum cache percentage that can be hit while running.''',
        type='category', subconfig=limit_stat)
]

transaction_config = [
    Config('min_operation_per_transaction', 1, r'''
        The minimum number of operations per transaction''', min=1, max=200000),
    Config('max_operation_per_transaction', 1, r'''
        The maximum number of operations per transaction''', min=1, max=200000),
]

timestamp_config = [
    Config('enable_timestamp', 'true', r'''
        Enables timestamp management''', type='boolean'),
    Config('oldest_lag', 0, r'''
        The duration between the stable and oldest timestamps''', min=0, max=1000000),
    Config('stable_lag', 0, r'''
        The duration between the latest and stable timestamps''', min=0, max=1000000),
]

logging_config = [
    Config('enable_logging', 'true', r'''
        Enables write ahead logs''', type='boolean'),
]

methods = {
    'poc_test' : Method(load_config + workload_config + runtime_monitor_config + transaction_config
        + timestamp_config + logging_config + test_config),
}
