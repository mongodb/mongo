"""Log the estimated remote execution spend of the EngFlow invocations a task ran.

EngFlow has no endpoint that reports dollars. Its server-side profile of an invocation does record
how long every remotely executed action occupied a worker and in which pool, so the spend is
estimated as

    worker-hours x vCPUs of a worker slot in that pool x blended $ per vCPU-hour

Remote cache hits cost nothing here because they never occupy a worker. Idle workers, storage and
network are not part of the estimate.

EngFlow keeps adding the actions that finished last to the profile for a while after the
invocation ends, so the profile is polled until it holds as many actions as bazel reports having
executed remotely.
"""

import json
import os
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from typing import Annotated, Callable, Iterable, Optional

import requests
import typer

# Pricing inputs. Change these when the rates or the fleet mix change.
SPOT_USD_PER_VCPU_HOUR = 0.13
ON_DEMAND_USD_PER_VCPU_HOUR = 0.22
# Share of worker time that runs on spot instances; the rest runs on demand.
SPOT_FRACTION = 0.70

# vCPUs of one worker slot in each remote execution pool. The test pools mirror
# POOLS_X86/POOLS_ARM in bazel/test_exec_properties.bzl; keep them in sync.
POOL_VCPUS = {
    "default": 1,
    "x86_64": 1,
    "large_mem_2core_x86_64": 2,
    "high_mem_2core_x86_64": 2,
    "test_runner_arm64_1core": 1,
    "test_runner_arm64_2core": 2,
    "test_runner_arm64_4core": 4,
    "arm_linker": 2,
}
# Pools missing from POOL_VCPUS are priced as this many vCPUs and called out in the report.
UNKNOWN_POOL_VCPUS = 1

ENGFLOW_CLUSTER = "sodalite.cluster.engflow.com"
PROFILE_URL = f"https://{ENGFLOW_CLUSTER}/api/profiling/v1/instances/default/invocations/{{}}"
FETCH_ATTEMPTS = 3
FETCH_RETRY_SECONDS = 10
# (connect, read) seconds. The profile of a full resmoke_tests run is tens of megabytes.
FETCH_TIMEOUT_SECONDS = (10, 180)
# How often and how many times to fetch a profile while waiting for EngFlow to record every
# remote action of the invocation. After the last fetch the estimate is reported as a lower bound.
PROFILE_POLL_SECONDS = 15
PROFILE_FETCHES = 13

app = typer.Typer(pretty_exceptions_show_locals=False)


class ProfileUnavailable(Exception):
    """EngFlow did not serve the profile of an invocation."""


@dataclass
class PoolUsage:
    """Remote execution an invocation did in one pool."""

    actions: int = 0
    worker_seconds: float = 0.0

    @property
    def worker_hours(self) -> float:
        return self.worker_seconds / 3600


@dataclass
class BepInfo:
    """What a --build_event_json_file says about its invocation."""

    invocation_id: str
    # Actions bazel executed remotely, or None when bazel did not get to write its build metrics.
    remote_actions: Optional[int]


@dataclass
class InvocationUsage:
    """Remote execution of one invocation, keyed by pool."""

    invocation_id: str
    pools: dict[str, PoolUsage]
    # Remote actions bazel reported for the invocation, when known.
    expected_actions: Optional[int] = None

    @property
    def recorded_actions(self) -> int:
        return sum(usage.actions for usage in self.pools.values())

    @property
    def is_complete(self) -> bool:
        """Whether EngFlow's profile holds every remote action bazel reported."""
        return self.expected_actions is None or self.recorded_actions >= self.expected_actions


def blended_usd_per_vcpu_hour(
    spot_usd_per_vcpu_hour: float = SPOT_USD_PER_VCPU_HOUR,
    on_demand_usd_per_vcpu_hour: float = ON_DEMAND_USD_PER_VCPU_HOUR,
    spot_fraction: float = SPOT_FRACTION,
) -> float:
    """Return the $ per vCPU-hour of the spot/on demand mix."""
    return (
        spot_fraction * spot_usd_per_vcpu_hour + (1 - spot_fraction) * on_demand_usd_per_vcpu_hour
    )


def pool_vcpus(pool: str) -> int:
    """Return the vCPUs of one worker slot in the pool."""
    return POOL_VCPUS.get(pool, UNKNOWN_POOL_VCPUS)


def pool_vcpu_hours(pool: str, usage: PoolUsage) -> float:
    return usage.worker_hours * pool_vcpus(pool)


def pool_cost_usd(pool: str, usage: PoolUsage) -> float:
    return pool_vcpu_hours(pool, usage) * blended_usd_per_vcpu_hour()


def summarize_worker_time(trace_events: Iterable[dict]) -> dict[str, PoolUsage]:
    """Sum the worker time of an EngFlow profile per pool.

    A worker_time event spans everything the action held the worker for: fetching inputs,
    executing and uploading outputs. Retried actions have one event per attempt.
    """
    pools: dict[str, PoolUsage] = defaultdict(PoolUsage)
    for event in trace_events:
        if event.get("cat") != "worker_time":
            continue
        usage = pools[event.get("args", {}).get("action_pool", "unknown")]
        usage.actions += 1
        usage.worker_seconds += event.get("dur", 0) / 1_000_000
    return dict(pools)


def read_bep(build_events_file: str) -> Optional[BepInfo]:
    """Return the invocation recorded in a --build_event_json_file, if any."""
    if not os.path.isfile(build_events_file):
        return None
    invocation_id = None
    remote_actions = None
    with open(build_events_file) as bep:
        for line in bep:
            # The BEP of a full run is large; only parse the two events that are needed.
            if '"started"' not in line and '"buildMetrics"' not in line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue  # A line cut short by a killed bazel.
            if "started" in event:
                invocation_id = event["started"].get("uuid")
            elif "buildMetrics" in event:
                runners = event["buildMetrics"].get("actionSummary", {}).get("runnerCount", [])
                remote_actions = sum(
                    int(runner.get("count", 0))
                    for runner in runners
                    if runner.get("name") == "remote"
                )
    if not invocation_id:
        return None
    return BepInfo(invocation_id, remote_actions)


def fetch_trace_events(
    session: requests.Session,
    invocation_id: str,
    sleep: Callable[[float], None] = time.sleep,
) -> list[dict]:
    """Download the EngFlow profile of an invocation and return its trace events."""
    url = PROFILE_URL.format(invocation_id)
    failure = ""
    for attempt in range(1, FETCH_ATTEMPTS + 1):
        try:
            response = session.get(url, timeout=FETCH_TIMEOUT_SECONDS)
            if response.status_code == 200:
                return response.json().get("traceEvents", [])
            failure = f"HTTP {response.status_code}"
        except (requests.RequestException, ValueError) as err:
            failure = str(err)
        if attempt < FETCH_ATTEMPTS:
            sleep(FETCH_RETRY_SECONDS)
    raise ProfileUnavailable(f"No EngFlow profile for invocation {invocation_id}: {failure}")


def fetch_invocation_usage(
    session: requests.Session,
    bep: BepInfo,
    fetch: Callable[[requests.Session, str], list[dict]] = fetch_trace_events,
    sleep: Callable[[float], None] = time.sleep,
) -> InvocationUsage:
    """Return the remote execution of an invocation, waiting for its profile to fill in."""
    for attempt in range(1, PROFILE_FETCHES + 1):
        usage = InvocationUsage(
            bep.invocation_id,
            summarize_worker_time(fetch(session, bep.invocation_id)),
            expected_actions=bep.remote_actions,
        )
        if usage.is_complete or attempt == PROFILE_FETCHES:
            break
        print(
            f"EngFlow has recorded {usage.recorded_actions} of {usage.expected_actions} remote "
            f"actions of invocation {bep.invocation_id}; checking again in "
            f"{PROFILE_POLL_SECONDS} seconds."
        )
        sleep(PROFILE_POLL_SECONDS)
    return usage


def make_session(cert: str, key: str) -> Optional[requests.Session]:
    """Return a session authenticated to EngFlow, or None when there are no credentials.

    Evergreen tasks hold a client certificate. Workstations have the engflow_auth credential
    helper instead.
    """
    session = requests.Session()
    if os.path.isfile(cert) and os.path.isfile(key):
        session.cert = (cert, key)
        return session

    helper = shutil.which("engflow_auth")
    if helper is None:
        return None
    result = subprocess.run(
        [helper, "get"],
        input=json.dumps({"uri": f"https://{ENGFLOW_CLUSTER}"}),
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        return None
    token = json.loads(result.stdout)["headers"]["x-engflow-auth-token"][0]
    session.headers["Authorization"] = f"Bearer {token}"
    return session


def format_report(invocations: list[InvocationUsage]) -> str:
    """Render the spend of a task's invocations as the message to log."""
    rate = blended_usd_per_vcpu_hour()
    total_vcpu_hours = sum(
        pool_vcpu_hours(pool, usage)
        for invocation in invocations
        for pool, usage in invocation.pools.items()
    )
    at_least = "" if all(invocation.is_complete for invocation in invocations) else "at least "
    lines = [
        f"Estimated remote execution spend of this task: {at_least}${total_vcpu_hours * rate:.2f} "
        f"({total_vcpu_hours:.1f} vCPU-hours at ${rate:.3f} per vCPU-hour: "
        f"{SPOT_FRACTION:.0%} spot at ${SPOT_USD_PER_VCPU_HOUR:.2f}, "
        f"{1 - SPOT_FRACTION:.0%} on demand at ${ON_DEMAND_USD_PER_VCPU_HOUR:.2f})"
    ]
    unknown_pools: set[str] = set()
    for invocation in invocations:
        cost = sum(pool_cost_usd(pool, usage) for pool, usage in invocation.pools.items())
        lines.append(f"  Invocation {invocation.invocation_id}: ${cost:.2f}")
        if not invocation.is_complete:
            lines.append(
                f"    Lower bound: EngFlow has only recorded {invocation.recorded_actions} of "
                f"{invocation.expected_actions} remote actions so far."
            )
        by_cost = sorted(invocation.pools.items(), key=lambda item: -pool_cost_usd(*item))
        for pool, usage in by_cost:
            lines.append(
                f"    {pool}: ${pool_cost_usd(pool, usage):.2f} "
                f"({usage.actions} actions, {usage.worker_hours:.1f} worker-hours "
                f"x {pool_vcpus(pool)} vCPU)"
            )
            if pool not in POOL_VCPUS:
                unknown_pools.add(pool)
    if unknown_pools:
        lines.append(
            f"  Pools priced as {UNKNOWN_POOL_VCPUS} vCPU because they are missing from "
            f"POOL_VCPUS in buildscripts/engflow_invocation_cost.py: "
            f"{', '.join(sorted(unknown_pools))}"
        )
    return "\n".join(lines)


@app.command()
def main(
    build_events_file: Annotated[
        list[str],
        typer.Option(
            help="--build_event_json_file of an invocation to price. Repeat for several "
            "invocations. Files that do not exist are skipped."
        ),
    ],
    cert: Annotated[str, typer.Option(help="EngFlow client certificate.")] = "engflow.cert",
    key: Annotated[str, typer.Option(help="EngFlow client key.")] = "engflow.key",
) -> None:
    """Log the estimated remote execution spend of the given invocations."""
    beps = [bep for bep in map(read_bep, build_events_file) if bep]
    if not beps:
        print("No bazel invocations to price; not estimating the remote execution spend.")
        return

    session = make_session(cert, key)
    if session is None:
        print("No EngFlow credentials found; not estimating the remote execution spend.")
        raise typer.Exit(code=1)

    invocations = []
    for bep in beps:
        try:
            invocations.append(fetch_invocation_usage(session, bep))
        except ProfileUnavailable as err:
            print(err)
            raise typer.Exit(code=1)

    print(format_report(invocations))


if __name__ == "__main__":
    sys.exit(app())
