import random
import time
from typing import List, Optional

from opentelemetry import trace
from opentelemetry.sdk.trace import IdGenerator


class ResmokeOtelIdGenerator(IdGenerator):
    """
    ID generator that creates unique span IDs across parallel resmoke.py invocations.

    This generator seeds Python's random module with a combination of:
    - Timestamp
    - Optional list of suites being run
    - Optional shard index (unique per parallel resmoke shard)

    This helps prevents ID collisions when multiple resmoke.py processes are
    run in parallel with the same traceID and parentSpanID.
    """

    def __init__(self, suite_files: Optional[List[str]] = None, shard_index: Optional[int] = None):
        """
        Initialize the unique span ID generator.

        Args:
            suite_files: Optional list of suites
            shard_index: Optional shard index to incorporate into the seed
        """

        seed_parts = [
            int(time.time() * 1_000),
        ]

        if suite_files is not None:
            seed_parts.append("".join(suite_files))

        if shard_index is not None:
            seed_parts.append(shard_index)

        seed = hash(tuple(seed_parts))

        # Create a separate Random instance to avoid interfering with other uses of random
        self._rng = random.Random(seed)

    def generate_span_id(self) -> int:
        """Generate a unique 64-bit span ID."""
        span_id = self._rng.getrandbits(64)
        while span_id == trace.INVALID_SPAN_ID:
            span_id = self._rng.getrandbits(64)
        return span_id

    def generate_trace_id(self) -> int:
        """Generate a unique 128-bit trace ID."""
        trace_id = self._rng.getrandbits(128)
        while trace_id == trace.INVALID_TRACE_ID:
            trace_id = self._rng.getrandbits(128)
        return trace_id
