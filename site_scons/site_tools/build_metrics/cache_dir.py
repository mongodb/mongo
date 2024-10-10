import os
import time
from typing import List, Set

from site_tools.validate_cache_dir import CacheDirValidate
from typing_extensions import TypedDict

from .protocol import BuildMetricsCollector


class CacheArtifact(TypedDict):
    array_index: int
    name: str
    size: int


class CacheMetrics(TypedDict):
    cache_artifacts: List[CacheArtifact]
    push_time: int
    pull_time: int
    cache_size: int


class CacheDirValidateWithMetrics(CacheDirValidate):
    DATA: CacheMetrics = CacheMetrics(push_time=0, pull_time=0, cache_artifacts=[], cache_size=0)
    SET: Set[str] = set()

    @classmethod
    def keep_stats(cls, target):
        if target in cls.SET:
            return

        size = os.path.getsize(target)
        cls.DATA["cache_artifacts"].append(
            CacheArtifact(array_index=len(cls.DATA["cache_artifacts"]), name=target, size=size)
        )
        cls.DATA["cache_size"] += size
        cls.SET.add(target)

    @classmethod
    def copy_from_cache(cls, env, src, dst):
        start = time.time_ns()
        super().copy_from_cache(env, src, dst)
        pull_time = time.time_ns() - start
        cls.DATA["pull_time"] += pull_time
        cls.keep_stats(dst)

    @classmethod
    def copy_to_cache(cls, env, src, dst):
        start = time.time_ns()
        super().copy_to_cache(env, src, dst)
        push_time = time.time_ns() - start
        cls.DATA["push_time"] += push_time
        cls.keep_stats(src)


class CacheDirCollector(BuildMetricsCollector):
    def get_name(self):
        return "CacheDirCollector"

    def finalize(self):
        return "cache_metrics", CacheDirValidateWithMetrics.DATA
