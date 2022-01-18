"""API for interacting with Cedar."""

from typing import Union, Dict, Any, List

import requests
import yaml


class CedarRollup:
    """Representation of Cedar rollup."""

    name: str
    val: Union[int, float]

    def __init__(self, json: Dict[str, Any]):
        """Initialize."""
        self.name = json["name"]
        self.val = json["val"]


class CedarPerfData:
    """Representation of Cedar performance data."""

    test_name: str
    thread_level: int
    perf_rollups: List[CedarRollup]

    def __init__(self, json: Dict[str, Any]):
        """Initialize."""
        self.test_name = json["info"]["test_name"]
        self.thread_level = json["info"]["args"]["thread_level"]
        self.perf_rollups = [CedarRollup(rollup) for rollup in json["rollups"]["stats"]]


class CedarApi:
    """Representation of Cedar API."""

    DEFAULT_API_SERVER = "https://cedar.mongodb.com"

    def __init__(self, evg_api_config: str):
        """Initialize."""
        with open(evg_api_config) as fh:
            evg_config = yaml.safe_load(fh)
        self.headers = {
            "Api-User": evg_config["user"],
            "Api-Key": evg_config["api_key"],
        }

    def get_perf_data_by_task_id(self, task_id: str) -> List[CedarPerfData]:
        """Get performance data by Evergreen task id."""
        url = f"{self.DEFAULT_API_SERVER}/rest/v1/perf/task_id/{task_id}"
        res = requests.get(url, headers=self.headers)
        res.raise_for_status()
        return [CedarPerfData(perf_data) for perf_data in res.json()]
