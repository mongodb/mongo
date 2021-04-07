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

#
# Record config specifies the format of the keys and values used in the database
#
record_config = [
    Config('key_size', 0, r'''
        The size of the keys created''', min=0, max=10000),
    Config('value_size', 0, r'''
        The size of the values created''', min=0, max=1000000000),
]

#
# The populate config defines how large the initially loaded database will be.
#
populate_config = [
    Config('collection_count', 1, r'''
        The number of collections the workload generator operates over''', min=0, max=200000),
    Config('key_count', 0, r'''
        The number of keys to be operated on per collection''', min=0, max=1000000),
]

#
# A generic configuration used by some components to define their tick rate.
#
throttle_config = [
    Config('rate_per_second',1,r'''
        The number of times an operation should be performed per second''', min=1,max=1000),
]

#
# A generic configuration used by various other configurations to define whether that component or
# similar is enabled or not.
#
enable_config = [
    Config('enabled', 'false', r'''
        Whether or not this is relevant to the workload''',
        type='boolean'),
]

stat_config = enable_config

limit_stat = stat_config + [
    Config('limit', 0, r'''
    The limit value a statistic is allowed to reach''')
]

range_config = [
    Config('min', 0, r'''
        The minimum a value can be in a range'''),
    Config('max', 1, r'''
        The maximum a value can be in a range''')
]

transaction_config = [
    Config('ops_per_transaction', '', r'''
        Defines how many operations a transaction can perform, the range is defined with a minimum
        and a maximum and a random number is chosen between the two using a linear distrubtion.''',
        type='category',subconfig=range_config),
]

#
# Configuration that applies to the runtime monitor component, this should be a list of statistics
# that need to be checked by the component.
#
runtime_monitor = throttle_config + [
    Config('stat_cache_size', '', '''
        The maximum cache percentage that can be hit while running.''',
        type='category', subconfig=limit_stat)
]

#
# Configuration that applies to the timestamp_manager component.
#
timestamp_manager = enable_config +  [
    Config('oldest_lag', 0, r'''
        The duration between the stable and oldest timestamps''', min=0, max=1000000),
    Config('stable_lag', 0, r'''
        The duration between the latest and stable timestamps''', min=0, max=1000000),
]

#
# Configuration that applies to the workload tracking component.
#
workload_tracking = enable_config

#
# Configuration that applies to the workload_generator component.
#
workload_generator = transaction_config + record_config + populate_config + [
    Config('read_threads', 0, r'''
        The number of threads performing read operations''', min=0, max=100),
    Config('insert_threads', 0, r'''
        The number of threads performing insert operations''', min=0, max=20),
    Config('insert_config', '', r'''
        The definition of the record being inserted, if record config is empty the top level
        record_config will be used.''',
        type='category', subconfig=record_config),
    Config('update_threads', 0, r'''
        The number of threads performing update operations''', min=0, max=20),
    Config('update_config', '',r'''
        The definition of the record being updated, if record config is empty the top level
        record_config will be used.''',
        type='category', subconfig=record_config)
]

test_config = [
# Component configurations.
    Config('runtime_monitor', '', r'''
        Configuration options for the runtime_monitor''',
        type='category', subconfig=runtime_monitor),
    Config('timestamp_manager', '', r'''
        Configuration options for the timestamp manager''',
        type='category', subconfig=timestamp_manager),
    Config('workload_generator','', r'''
        Configuration options for the workload generator''',
        type='category', subconfig=workload_generator),
    Config('workload_tracking','', r'''
        Configuration options for the workload tracker''',
        type='category', subconfig=workload_tracking),

# Non component top level configuration.
    Config('cache_size_mb', 0, r'''
        The cache size that wiredtiger will be configured to run with''', min=0, max=100000000000),
    Config('duration_seconds', 0, r'''
        The duration that the test run will last''', min=0, max=1000000),
    Config('enable_logging', 'true', r'''
        Enables write ahead logs''', type='boolean'),
]

methods = {
    'example_test' : Method(test_config),
    'poc_test' : Method(test_config),
}
