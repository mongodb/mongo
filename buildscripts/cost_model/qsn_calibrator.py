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
"""Calibrate QSN nodes."""

from __future__ import annotations

import experiment as exp
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
import statsmodels.api as sm
from config import QsNodeCalibrationConfig, QuerySolutionCalibrationConfig
from cost_estimator import estimate
from database_instance import DatabaseInstance
from sklearn.linear_model import LinearRegression

__all__ = ["calibrate"]


async def calibrate(config: QuerySolutionCalibrationConfig, database: DatabaseInstance):
    """Main entry-point for QSN calibration."""

    if not config.enabled:
        return {}

    df = await exp.load_calibration_data(database, config.input_collection_name)
    noout_df = exp.remove_outliers(df, 0.1, 0.90)
    qsn_df = exp.extract_qsn_nodes(noout_df)
    result = {}
    for node_config in config.nodes:
        key = node_config.name if node_config.name else node_config.type
        result[key] = calibrate_node(qsn_df, config, node_config)
    return result


def calibrate_node(
    qsn_df: pd.DataFrame,
    config: QuerySolutionCalibrationConfig,
    node_config: QsNodeCalibrationConfig,
):
    node_name = node_config.name if node_config.name else node_config.type
    qsn_node_df = qsn_df[
        (qsn_df.node_type == node_config.type) & (qsn_df.note.isna() | (qsn_df.note == node_name))
    ]
    if node_config.filter_function is not None:
        qsn_node_df = node_config.filter_function(qsn_node_df)
    y = qsn_node_df["execution_time"]
    X_vars = (
        pd.DataFrame({"Number of documents": qsn_node_df["n_processed"]})
        if node_config.variables_override is None
        else node_config.variables_override(qsn_node_df)
    )
    n_vars = X_vars.shape[1]
    labels = X_vars.columns.tolist()
    X = sm.add_constant(X_vars)

    def fit(X, y):
        nnls = LinearRegression(positive=True, fit_intercept=False)
        model = nnls.fit(X, y)
        return (model.coef_, model.predict)

    model = estimate(fit, X.to_numpy(), y.to_numpy(), config.test_size, config.trace)
    # plot regression and save to file
    if model.predict:
        if n_vars == 2:
            fig = plt.figure()
            ax = fig.add_subplot(111, projection="3d")
            X1 = X_vars.iloc[:, 0]
            X2 = X_vars.iloc[:, 1]
            ax.scatter(X1, X2, y, label="Executions", color="blue")

            x1_range = np.linspace(X1.min(), X1.max())
            x2_range = np.linspace(X2.min(), X2.max())
            x1_mesh, x2_mesh = np.meshgrid(x1_range, x2_range)

            ax.plot_surface(
                x1_mesh,
                x2_mesh,
                model.coef[0] * x1_mesh + model.coef[1] * x2_mesh + model.intercept,
                alpha=0.5,
                color="orange",
                label="Regression Plane",
            )

            ax.set(
                xlabel=labels[0],
                ylabel=labels[1],
                zlabel="Execution time (ns)",
                title=f"Regression for {node_name}",
            )
            ax.legend()
        elif n_vars == 1:
            fig, ax = plt.subplots()
            ax.scatter(X_vars, y, label="Executions")
            ax.plot(
                X_vars,
                model.predict(X),
                linewidth=3,
                color="tab:orange",
                label="Linear Regression",
            )
            ax.set(
                xlabel=labels[0],
                ylabel="Execution time (ns)",
                title=f"Regression for {node_name}",
            )

            ax.legend()
        else:
            raise ValueError(f"Currently only support 1 or 2 input variables, got {n_vars}")

        if fig:
            fig.savefig(f"{node_name}.png")
    return model
