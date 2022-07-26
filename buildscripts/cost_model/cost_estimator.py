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
from typing import Sequence
from sklearn import linear_model
from sklearn.metrics import mean_squared_error, r2_score, explained_variance_score
from sklearn.model_selection import train_test_split
from workload_execution import QueryParameters


@dataclass
class ExecutionStats:
    """Exection Statistics."""

    execution_time: int
    n_returned: int
    n_processed: int


@dataclass
class ModelParameters:
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


def estimate(params: Sequence[ModelParameters], test_size: float, trace: bool = False):
    """Estimate cost model parameters from the given statistics of SBE stage."""
    # pylint: disable=invalid-name
    # clean data
    params = list(filter(lambda s: s.execution_stats.n_processed > 0, params))

    # prepare data
    X = [[
        s.execution_stats.n_processed, s.query_params.average_document_size_in_bytes,
        s.query_params.keys_length_in_bytes
    ] for s in params]
    y = [s.execution_stats.execution_time for s in params]

    # split data
    X_training, X_test, y_training, y_test = train_test_split(X, y, test_size=test_size)

    if trace:
        print(f'Training size: {len(X_training)}, test size: {len(X_test)}')
        print(X_training)
        print(y_training)

    if len(X_test) == 0 or len(X_training) == 0:
        # no data to trainn return empty model
        return LinearModel(coef=[], intercept=0, mse=0, rs=0, evs=0)

    # train
    reg = linear_model.LinearRegression()
    reg.fit(X_training, y_training)

    # Error estimation
    y_predict = reg.predict(X_test)

    mse = mean_squared_error(y_test, y_predict)
    r2 = r2_score(y_test, y_predict)
    evs = explained_variance_score(y_test, y_predict)

    return LinearModel(coef=reg.coef_, intercept=reg.intercept_, mse=mse, r2=r2, evs=evs)
