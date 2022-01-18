"""Google Benchmarks analyzer config."""
import os
from typing import Dict, Union

import yaml

THRESHOLD_CONFIG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "threshold_config.yml")


def get_thresholds(suite: str) -> Dict[str, Union[int, float]]:
    """Get thresholds configured in  yaml file."""

    with open(THRESHOLD_CONFIG) as fh:
        cfg = yaml.safe_load(fh)

    thresholds = cfg["defaults"]
    overrides = cfg["overrides"].get(suite)
    if overrides:
        for key, value in overrides.items():
            thresholds[key] = value

    return thresholds
