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
"""Common cost estimator types and functions."""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from sklearn.metrics import explained_variance_score, mean_squared_error, r2_score
from sklearn.model_selection import train_test_split
from workload_execution import QueryParameters


@dataclass
class ExecutionStats:
    """Exection Statistics."""

    execution_time: int
    n_returned: int
    n_processed: int


@dataclass
class CostModelParameters:
    """Cost Model Input Parameters."""

    execution_stats: ExecutionStats
    query_params: QueryParameters


@dataclass
class LinearModel:
    """Calibrated Linear Model and its metrics."""

    # pylint: disable=invalid-name
    intercept: float
    coef: list[float]
    mse: float  # Mean Squared Error
    r2: float  # Coefficient of determination
    evs: float  # Explained Variance Score
    corrcoef: any  # Correlation Coefficients


# pylint: disable=invalid-name
def estimate(
    fit, X: np.ndarray, y: np.ndarray, test_size: float, trace: bool = False
) -> LinearModel:
    """Estimate cost model parameters."""

    if len(X) == 0:
        # no data to trainn return empty model
        return LinearModel(coef=[], intercept=0, mse=0, r2=0, evs=0, corrcoef=[])

    # split data
    X_training, X_test, y_training, y_test = train_test_split(X, y, test_size=test_size)

    if trace:
        print(f"Training size: {len(X_training)}, test size: {len(X_test)}")
        print(X_training)
        print(y_training)

    if len(X_test) == 0 or len(X_training) == 0:
        # no data to trainn return empty model
        return LinearModel(coef=[], intercept=0, mse=0, r2=0, evs=0, corrcoef=[])

    (coef, predict) = fit(X, y)
    y_predict = predict(X_test)

    mse = mean_squared_error(y_test, y_predict)
    r2 = r2_score(y_test, y_predict)
    evs = explained_variance_score(y_test, y_predict)
    corrcoef = np.corrcoef(np.transpose(X[:, 1:]), y)

    return LinearModel(
        coef=coef[1:], intercept=coef[0], mse=mse, r2=r2, evs=evs, corrcoef=corrcoef[0, 1:]
    )
