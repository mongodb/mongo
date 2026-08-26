#!/usr/bin/env python3
"""Activate an evergreen task in the existing build.

This script is uploaded to S3 once per version by `version_gen` and downloaded by every '*_gen'
task, so that those tasks need neither a source checkout, a virtualenv, nor the dist test archive.
  1. It must import only the Python standard library. There is no virtualenv on the host, so
     neither the evergreen.py client nor anything else from etc/pip is available. The small
     Evergreen REST client at the bottom of this file stands in for evergreen.py.
  2. It must be a single self-contained file. Nothing else from the repo is on the host, so
     buildscripts.util.taskname.remove_gen_suffix is duplicated here.
"""

import http.client
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, Iterator, List, NamedTuple, Optional, Tuple

GEN_SUFFIX = "_gen"
DEFAULT_API_SERVER = "https://evergreen.mongodb.com/api"
BURN_IN_TAGS = "burn_in_tags"
BURN_IN_TESTS = "burn_in_tests"
BURN_IN_VARIANT_SUFFIX = "generated-by-burn-in-tags"


class EvgExpansions(NamedTuple):
    """
    Evergreen expansions, passed through the environment.

    build_id: ID of build being run.
    version_id: ID of version being run.
    task_name: Name of task creating the generated configuration.
    api_user: Evergreen API user.
    api_key: Evergreen API key.
    api_server: Evergreen API server.
    """

    build_id: str
    version_id: str
    task_name: str
    api_user: str
    api_key: str
    api_server: str

    @classmethod
    def from_environ(cls, environ: Dict[str, str]) -> "EvgExpansions":
        """Read the expansions, failing with a clear message if Evergreen did not pass one."""
        # Windows environment variables are case-insensitive, and Python upper-cases every key of
        # os.environ there, so the lower-case expansion names must be looked up case-insensitively.
        environ_lowercase = {name.lower(): value for name, value in environ.items()}
        missing = [
            name
            for name in (
                "build_id",
                "version_id",
                "task_name",
                "evergreen_api_user",
                "evergreen_api_key",
            )
            if not environ_lowercase.get(name)
        ]
        if missing:
            raise KeyError(f"Missing required expansion(s) in the environment: {sorted(missing)}")
        return cls(
            build_id=environ_lowercase["build_id"],
            version_id=environ_lowercase["version_id"],
            task_name=environ_lowercase["task_name"],
            api_user=environ_lowercase["evergreen_api_user"],
            api_key=environ_lowercase["evergreen_api_key"],
            api_server=environ_lowercase.get("evergreen_api_server") or DEFAULT_API_SERVER,
        )

    @property
    def task(self) -> str:
        """Get the task being generated."""
        return remove_gen_suffix(self.task_name)


def activate_task(expansions: "EvgExpansions", evg_api: "SimpleEvergreenApi") -> None:
    """
    Activate the given task in the specified build.

    :param expansions: Evergreen expansions.
    :param evg_api: Evergreen API client.
    """
    tasks_not_activated = []
    if expansions.task == BURN_IN_TAGS:
        version = evg_api.version_by_id(expansions.version_id)
        for build_variant in version.build_variants_map.keys():
            build_id = version.build_variants_map[build_variant]
            task_list = evg_api.tasks_by_build(build_id)
            if build_variant.endswith(BURN_IN_VARIANT_SUFFIX):
                for task in task_list:
                    if task.display_name == BURN_IN_TESTS:
                        print(f"Activating task {task.display_name} ({task.task_id})")
                        try:
                            evg_api.configure_task(task.task_id, activated=True)
                        except Exception as err:
                            print(
                                f"Could not activate task {task.display_name} "
                                f"({task.task_id}): {err}",
                                file=sys.stderr,
                            )
                            tasks_not_activated.append(task.task_id)
            else:
                for task in task_list:
                    if re.match(BURN_IN_TESTS, task.display_name):
                        print(f"Activating task {task.display_name} ({task.task_id})")
                        try:
                            evg_api.configure_task(task.task_id, activated=True)
                        except Exception as err:
                            print(
                                f"Could not activate task {task.display_name} "
                                f"({task.task_id}): {err}",
                                file=sys.stderr,
                            )
                            tasks_not_activated.append(task.task_id)

    else:
        task_list = evg_api.tasks_by_build(expansions.build_id)
        for task in task_list:
            if task.display_name == expansions.task:
                print(f"Activating task {task.display_name} ({task.task_id})")
                try:
                    evg_api.configure_task(task.task_id, activated=True)
                except Exception as err:
                    print(
                        f"Could not activate task {task.display_name} ({task.task_id}): {err}",
                        file=sys.stderr,
                    )
                    tasks_not_activated.append(task.task_id)
    if len(tasks_not_activated) > 0:
        print(
            f"Some tasks were unable to be activated: {len(tasks_not_activated)}", file=sys.stderr
        )
        raise ValueError(
            "Some tasks were unable to be activated, failing the task to let the author know. "
            "This should not be a blocking issue but may mean that some tasks are missing from your patch."
        )


TASKS_PER_PAGE = 1000

REQUEST_TIMEOUT_SECONDS = 60

RETRY_ATTEMPTS = 3
RETRY_DELAY_SECONDS = 5
RETRY_BACKOFF = 2

# A page of tasks cannot cycle forever; bound the walk in case a Link header ever points backwards.
# Running out of pages is treated as an error rather than as an empty result, because reporting a
# truncated task list would silently look like "the generated task does not exist".
MAX_PAGES = 100


def remove_gen_suffix(task_name: str) -> str:
    """Remove the '_gen' suffix from a task name."""
    if task_name.endswith(GEN_SUFFIX):
        return task_name[: -len(GEN_SUFFIX)]
    return task_name


class Task(NamedTuple):
    """The fields of an Evergreen task this script looks at."""

    display_name: str
    task_id: str


class Version(NamedTuple):
    """The fields of an Evergreen version this script looks at."""

    build_variants_map: Dict[str, str]


class SimpleEvergreenApi:
    """The few Evergreen REST v2 calls this script makes, over urllib."""

    def __init__(self, expansions: "EvgExpansions"):
        self.expansions = expansions

    def _request(
        self, url: str, method: str = "GET", body: Any = None
    ) -> Tuple[Any, Optional[str]]:
        """Make one API call, returning the decoded JSON body and the URL of the next page."""
        if url.startswith("https://") and not _urllib_speaks_https():
            return self._request_via_curl(url, method=method, body=body)

        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(url=url, data=data, method=method)
        request.add_header("Api-User", self.expansions.api_user)
        request.add_header("Api-Key", self.expansions.api_key)
        if data is not None:
            request.add_header("Content-Type", "application/json")

        with urllib.request.urlopen(request, timeout=REQUEST_TIMEOUT_SECONDS) as response:
            payload = response.read()
            parsed = json.loads(payload) if payload else None
            return parsed, _next_page_url(response.headers.get("Link"))

    def _request_via_curl(self, url: str, method: str, body: Any) -> Tuple[Any, Optional[str]]:
        """Make one API call with curl, for interpreters whose urllib cannot speak HTTPS."""
        curl = shutil.which("curl") or shutil.which("curl.exe")
        if curl is None:
            raise OSError(
                f"{sys.executable} was built without the ssl module, so urllib cannot request "
                f"{url}, and no curl was found on PATH to fall back to."
            )

        with tempfile.TemporaryDirectory() as tempdir:
            headers_path = os.path.join(tempdir, "headers.txt")
            # The credentials go in a config file on stdin rather than in argv, where any other
            # process on the host could read them out of the process list.
            config = [
                f"url = {_curl_value(url)}",
                f"request = {_curl_value(method)}",
                f"header = {_curl_value('Api-User: ' + self.expansions.api_user)}",
                f"header = {_curl_value('Api-Key: ' + self.expansions.api_key)}",
                f"dump-header = {_curl_value(headers_path)}",
                f"max-time = {_curl_value(str(REQUEST_TIMEOUT_SECONDS))}",
                "silent",
                "show-error",
            ]
            if body is not None:
                body_path = os.path.join(tempdir, "body.json")
                with open(body_path, "wb") as body_file:
                    body_file.write(json.dumps(body).encode())
                config.append(f"data = {_curl_value('@' + body_path)}")
                config.append(f"header = {_curl_value('Content-Type: application/json')}")

            process = subprocess.run(
                [curl, "--config", "-"],
                input="\n".join(config).encode(),
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if process.returncode != 0:
                raise OSError(
                    f"curl exited {process.returncode} requesting {url}: "
                    f"{process.stderr.decode(errors='replace').strip()}"
                )
            with open(headers_path, "r", errors="replace") as headers_file:
                status, link_header = _parse_curl_headers(headers_file.read())

        if not 200 <= status < 300:
            # Raised as an OSError so that _request_with_retries treats it the same way it treats
            # the urllib.error.HTTPError the urllib path raises here.
            raise OSError(f"Evergreen API returned HTTP {status} for {url}")

        parsed = json.loads(process.stdout) if process.stdout else None
        return parsed, _next_page_url(link_header)

    def _request_with_retries(self, url: str, method: str = "GET", body: Any = None) -> Any:
        """Make one API call, retrying transient failures with a backoff."""
        delay = RETRY_DELAY_SECONDS
        for attempt in range(1, RETRY_ATTEMPTS + 1):
            try:
                return self._request(url, method=method, body=body)
            # OSError covers urllib.error.URLError and TimeoutError. http.client.HTTPException
            # covers a body that is cut off mid-read (IncompleteRead), which urllib does not wrap
            # in a URLError; that is the failure this script used to retry explicitly, as
            # requests.exceptions.ChunkedEncodingError.
            except (OSError, http.client.HTTPException, json.JSONDecodeError) as err:
                print(f"Evergreen API call failed on attempt {attempt}: {err}", file=sys.stderr)
                if attempt == RETRY_ATTEMPTS:
                    raise
                time.sleep(delay)
                delay *= RETRY_BACKOFF

    def version_by_id(self, version_id: str) -> "Version":
        """Get the version, with a map of build variant name to build id for its variants."""
        page, _ = self._request_with_retries(
            f"{self.expansions.api_server}/rest/v2/versions/{urllib.parse.quote(version_id)}"
        )
        return Version(
            build_variants_map={
                build["build_variant"]: build["build_id"]
                for build in (page or {}).get("build_variants_status") or []
            }
        )

    def tasks_by_build(self, build_id: str) -> Iterator["Task"]:
        """Yield every task in the build, following Evergreen's pagination."""
        url = (
            f"{self.expansions.api_server}/rest/v2/builds/{urllib.parse.quote(build_id)}"
            f"/tasks?limit={TASKS_PER_PAGE}"
        )
        for _ in range(MAX_PAGES):
            page, next_url = self._request_with_retries(url)
            for task in page or []:
                yield Task(display_name=task["display_name"], task_id=task["task_id"])
            if not next_url:
                return
            url = next_url
        raise RuntimeError(
            f"Still being offered more pages of tasks for build {build_id} after {MAX_PAGES} of "
            "them. Refusing to go on, because a truncated task list would look exactly like the "
            "generated task not existing."
        )

    def configure_task(self, task_id: str, activated: bool) -> None:
        """Configure a task that is already in the build."""
        self._request_with_retries(
            f"{self.expansions.api_server}/rest/v2/tasks/{urllib.parse.quote(task_id)}",
            method="PATCH",
            body={"activated": activated},
        )


def _urllib_speaks_https() -> bool:
    """Report whether urllib can make HTTPS requests."""
    return hasattr(urllib.request, "HTTPSHandler")


def _curl_value(value: str) -> str:
    """Quote a value for a curl config file, which takes C-style escapes inside double quotes."""
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _parse_curl_headers(dumped: str) -> Tuple[int, Optional[str]]:
    """Read the status code and the Link header out of a curl --dump-header file."""
    lines = dumped.replace("\r\n", "\n").split("\n")
    status_line = lines[0] if lines else ""
    match = re.match(r"HTTP/\S+\s+(\d{3})", status_line)
    if not match:
        raise OSError(f"Could not read a status code out of curl's response: {status_line!r}")

    link_values: List[str] = []
    for line in lines[1:]:
        name, separator, value = line.partition(":")
        if separator and name.strip().lower() == "link":
            link_values.append(value.strip())
    return int(match.group(1)), ", ".join(link_values) or None


def _next_page_url(link_header: Optional[str]) -> Optional[str]:
    """Return the URL of the next page from a Link header, or None on the last page."""
    if not link_header:
        return None
    for link in link_header.split(","):
        parts = link.split(";")
        if any(part.strip() == 'rel="next"' for part in parts[1:]):
            return parts[0].strip().lstrip("<").rstrip(">")
    return None


def main() -> None:
    """Activate the associated generated executions based in the running build."""
    expansions = EvgExpansions.from_environ(dict(os.environ))
    evg_api = SimpleEvergreenApi(expansions)

    activate_task(expansions, evg_api)


if __name__ == "__main__":
    main()
