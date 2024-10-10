from typing import Any, List, Optional, Tuple

import SCons.Script
from typing_extensions import TypedDict

from .protocol import BuildMetricsCollector


class _HookedStartTime(float):
    def __init__(self, val) -> None:
        float.__init__(val)
        self.hooked_end_time = None

    def __rsub__(self, other):
        self.hooked_end_time = other
        return other - float(self)


def _safe_list_get(list_, i, default=None):
    try:
        return list_[i]
    except IndexError:
        return default


class MemoryMetrics(TypedDict):
    pre_read: int
    post_read: int
    pre_build: int
    post_build: int


class TimeMetrics(TypedDict):
    total: int
    sconscript_exec: int
    scons_exec: int
    command_exec: int


class CountsMetrics(TypedDict):
    array_index: int
    item_name: str
    pre_read: int
    post_read: int
    pre_build: int
    post_build: int


class SConsStats(BuildMetricsCollector):
    def __init__(self):
        # hook start_time so we can also capture the end time
        if not isinstance(SCons.Script.start_time, _HookedStartTime):
            SCons.Script.start_time = _HookedStartTime(SCons.Script.start_time)

    def get_name(self) -> str:
        return "SConsStats"

    def finalize(self) -> Tuple[str, Any]:
        out = {}
        memory = self._finalize_memory()
        if memory is not None:
            out["memory"] = memory
        time = self._finalize_time()
        if time is not None:
            out["time"] = time
        counts = self._finalize_counts()
        if counts is not None:
            out["counts"] = counts
        return "scons_metrics", out

    def _finalize_memory(self) -> Optional[MemoryMetrics]:
        memory_stats = SCons.Script.Main.memory_stats.stats
        pre_read = _safe_list_get(memory_stats, 0, 0)
        post_read = _safe_list_get(memory_stats, 1, 0)
        pre_build = _safe_list_get(memory_stats, 2, 0)
        post_build = _safe_list_get(memory_stats, 3, 0)
        if pre_read == 0 and post_read == 0 and pre_build == 0 and post_build == 0:
            print(
                "WARNING: SConsStats read all memory statistics as 0. Did you pass --debug=memory?"
            )
            return None
        return MemoryMetrics(
            pre_read=pre_read, post_read=post_read, pre_build=pre_build, post_build=post_build
        )

    def _finalize_counts(self) -> Optional[List[CountsMetrics]]:
        count_stats = SCons.Script.Main.count_stats.stats
        if len(count_stats) != 4:
            print(
                f"WARNING: SConsStats expected 4 counts, found {len(count_stats)}. Did you pass --debug=count?"
            )
            return None

        # This incomprehensible block taken from SCons produces stats_table,
        # a mapping of class name to a list of counts with the same order as
        # count_stats.labels
        # From SCons/Script/Main.py:517
        stats_table = {}
        for s in count_stats:
            for n in [t[0] for t in s]:
                stats_table[n] = [0, 0, 0, 0]
        i = 0
        for s in count_stats:
            for n, c in s:
                stats_table[n][i] = c
            i = i + 1
        # End section copied from SCons

        out = []
        for key, value in stats_table.items():
            out.append(
                CountsMetrics(
                    array_index=len(out),
                    item_name=key,
                    pre_read=value[0],
                    post_read=value[1],
                    pre_build=value[2],
                    post_build=value[3],
                )
            )

        return out

    def _finalize_time(self) -> Optional[TimeMetrics]:
        # unfortunately, much of the SCons time keeping is encased in the
        # main() function with local variables, so we're stuck copying
        # a bit of logic from SCons.Script.Main

        end_time = SCons.Script.start_time.hooked_end_time
        try:
            total_time = end_time - SCons.Script.start_time
        except TypeError as e:
            if str(e) == "unsupported operand type(s) for -: 'NoneType' and 'float'":
                print(
                    "WARNING: SConsStats failed to calculate SCons total time. Did you pass --debug=time?"
                )
                return None
            raise e

        sconscript_time = SCons.Script.Main.sconscript_time

        # From SCons/Script/Main.py:1428
        if SCons.Script.Main.num_jobs == 1:
            ct = SCons.Script.Main.cumulative_command_time
        else:
            if (
                SCons.Script.Main.last_command_end is None
                or SCons.Script.Main.first_command_start is None
            ):
                ct = 0.0
            else:
                ct = SCons.Script.Main.last_command_end - SCons.Script.Main.first_command_start
        scons_time = total_time - sconscript_time - ct
        # End section copied from SCons

        return TimeMetrics(
            total=total_time,
            sconscript_exec=sconscript_time,
            scons_exec=scons_time,
            command_exec=ct,
        )
