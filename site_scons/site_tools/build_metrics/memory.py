import threading
import time
import psutil
import sys
from .util import timestamp_now
from .protocol import BuildMetricsCollector


class MemoryMonitor(BuildMetricsCollector):
    INTERVAL = 0.1  # seconds

    def __init__(self, starting_memory_adjustment=0):
        self._stop = False
        self.system_memory_metrics = {
            "mem_over_time": [],
            "start_mem": used_memory() - starting_memory_adjustment,
        }

        self._thread = threading.Thread(target=self.memory_monitor, daemon=True)
        self._thread.start()

    def finalize(self):
        self._stop = True
        self._record_data_point()

        mean = 0
        max_ = 0
        count = 1
        for val in self.system_memory_metrics["mem_over_time"]:
            max_ = max(val["memory"], max_)
            # iterative mean calculation algorithm from https://stackoverflow.com/a/1934266
            mean += (val["memory"] - mean) / count
            count += 1

        self.system_memory_metrics["arithmetic_mean"] = mean
        self.system_memory_metrics["max"] = max_

        return 'system_memory', self.system_memory_metrics

    def memory_monitor(self):
        while not self._stop:
            time.sleep(self.INTERVAL)
            if self._stop:
                break

            self._record_data_point()

    def _record_data_point(self):
        used_mem = used_memory()
        now_time = timestamp_now()

        self.system_memory_metrics["mem_over_time"].append(
            {"timestamp": now_time, "memory": used_mem})


def used_memory():
    return psutil.virtual_memory().used
