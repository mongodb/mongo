import re
from perf_stat import PerfStat


def find_stat(test_stat_path: str, pattern: str, position_of_value: int):
    for line in open(test_stat_path):
        match = re.match(pattern, line)
        if match:
            return float(line.split()[position_of_value])
    return 0


class PerfStatCollection:
    def __init__(self):
        self.perf_stats = {}

    def add_stat(self, perf_stat: PerfStat):
        self.perf_stats[perf_stat.short_label] = perf_stat

    def find_stats(self, test_stat_path: str):
        for stat in self.perf_stats.values():
            value = find_stat(test_stat_path=test_stat_path,
                              pattern=stat.pattern,
                              position_of_value=stat.input_offset)
            stat.add_value(value=value)

    def to_value_list(self):
        as_list = []
        for stat in self.perf_stats.values():
            as_list.append({
                'name': stat.output_label,
                'value': stat.get_core_average(),
                'values': stat.values
            })
        return as_list

    def to_dict(self):
        as_dict = {'metrics': self.to_value_list()}
        return as_dict
