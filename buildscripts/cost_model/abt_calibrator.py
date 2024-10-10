# Copyright (C) 2022-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
#
"""Calibrate ABT nodes."""

from __future__ import annotations

import experiment as exp
import pandas as pd
import statsmodels.api as sm
from config import AbtCalibratorConfig, AbtNodeCalibrationConfig
from cost_estimator import estimate
from database_instance import DatabaseInstance
from sklearn.linear_model import LinearRegression

__all__ = ["calibrate"]


async def calibrate(config: AbtCalibratorConfig, database: DatabaseInstance):
    """Main entry-point for ABT calibration."""

    if not config.enabled:
        return {}

    df = await exp.load_calibration_data(database, config.input_collection_name)
    noout_df = exp.remove_outliers(df, 0.0, 0.90)
    abt_df = exp.extract_abt_nodes(noout_df)
    result = {}
    for node_config in config.nodes:
        result[node_config.type] = calibrate_node(abt_df, config, node_config)
    return result


def calibrate_node(
    abt_df: pd.DataFrame, config: AbtCalibratorConfig, node_config: AbtNodeCalibrationConfig
):
    abt_node_df = abt_df[abt_df.abt_type == node_config.type]
    if node_config.filter_function is not None:
        abt_node_df = node_config.filter_function(abt_node_df)

    # pylint: disable=invalid-name
    if node_config.variables_override is None:
        variables = ["n_processed"]
    else:
        variables = node_config.variables_override
    y = abt_node_df["execution_time"]
    X = abt_node_df[variables]

    X = sm.add_constant(X)

    def fit(X, y):
        nnls = LinearRegression(positive=True, fit_intercept=False)
        model = nnls.fit(X, y)
        return (model.coef_, model.predict)

    return estimate(fit, X.to_numpy(), y.to_numpy(), config.test_size, config.trace)
