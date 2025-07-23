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
"""Experimenation utility functions.

How to use the utility functions.

First of all we need to run Jupiter Notebook:
sh> python3 -m notebook

Example notebook.

#Imports
import math
import seaborn as sns
import statsmodels.api as sm

import sys
sys.path.append('/home/ubuntu/mongo/buildscripts/cost_model')
import experiment as exp
from config import DatabaseConfig
from database_instance import DatabaseInstance

# Load data
database_config = DatabaseConfig(connection_string='mongodb://localhost',
                                     database_name='abt_calibration', dump_path='',
                                     restore_from_dump=False, dump_on_exit=False)

database = DatabaseInstance(database_config)
df = await exp.load_calibration_data(database, 'calibrationData')

# Descriptive functions
df.describe()
df.head()

# Clean up loaded data
noout_df = exp.remove_outliers(df, 0.0, 0.90)
noout_df.describe()

# Extract ABT nodes
abt_df = exp.extract_abt_nodes(noout_df)
abt_df.head()

# Get IndexScan nodes only.
ixscan_df = abt_df[abt_df['abt_type'] == 'IndexScan']
ixscan_df.describe()

# Add a new column if required.
ixscan_df = ixscan_df[ixscan_df['n_processed'] > 0].copy()
ixscan_df['log_n_processed'] = ixscan_df['n_processed'].apply(math.log)
ixscan_df.describe()

# Check the correlation.
ixscan_df.corr()

# Print a scatter plot to see a dependency between e.g. execution_time and n_processed.
sns.scatterplot(x=ixscan_df['n_processed'], y=ixscan_df['execution_time'])

# Calibrate (train) the cost model for IndexScan
y = ixscan_df['execution_time']
X = ixscan_df[['n_processed', 'keys_length_in_bytes', 'average_document_size_in_bytes', 'log_n_processed']]
X = sm.add_constant(X)
ixscan_lm = sm.OLS(y, X).fit()
ixscan_lm.summary()

# Draw the predictions.
y_pred = ixscan_lm.predict(X)
sns.scatterplot(x=ixscan_df['n_processed'], y=ixscan_df['execution_time'])
sns.lineplot(x=ixscan_df['n_processed'],y=y_pred, color='red')
"""

from __future__ import annotations

import dataclasses

import bson.json_util as json
import execution_tree_classic as classic
import pandas as pd
import query_solution_tree as qsn
import seaborn as sns
import statsmodels.api as sm
from database_instance import DatabaseInstance
from parameters_extractor_classic import get_execution_stats
from sklearn.linear_model import LinearRegression
from sklearn.metrics import r2_score


async def load_calibration_data(database: DatabaseInstance, collection_name: str) -> pd.DataFrame:
    """Load workflow data containing explain output from database and parse it. Retuned calibration DataFrame with parsed SBE and ABT."""

    data = await database.get_all_documents(collection_name)
    df = pd.DataFrame(data)
    df["classic"] = df.explain.apply(
        lambda e: classic.build_execution_tree(json.loads(e)["executionStats"])
    )
    df["qsn"] = df.explain.apply(lambda e: qsn.build(json.loads(e)["queryPlanner"]["winningPlan"]))
    df["total_execution_time"] = df.classic.apply(lambda t: t.execution_time_nanoseconds)
    return df


def remove_outliers(
    df: pd.DataFrame, lower_percentile: float = 0.1, upper_percentile: float = 0.9
) -> pd.DataFrame:
    """Remove the outliers from the parsed calibration DataFrame."""

    def is_not_outlier(df_seq):
        low = df_seq.quantile(lower_percentile)
        high = df_seq.quantile(upper_percentile)
        return (df_seq >= low) & (df_seq <= high)

    return df[
        df.groupby(["run_id", "collection", "command"])
        .total_execution_time.transform(is_not_outlier)
        .eq(1)
    ]


def extract_sbe_stages(df: pd.DataFrame) -> pd.DataFrame:
    """Extract SBE stages from calibration DataFrame."""

    def flatten_sbe_stages(explain):
        def traverse(node, stages):
            execution_time = node["executionTimeNanos"]
            children_fields = ["innerStage", "outerStage", "inputStage", "thenStage", "elseStage"]
            for field in children_fields:
                if field in node and node[field]:
                    child = node[field]
                    execution_time -= child["executionTimeNanos"]
                    traverse(child, stages)
                    del node[field]
            node["executionTime"] = execution_time
            stages.append(node)

        sbe_tree = json.loads(explain)["executionStats"]["executionStages"]
        result = []
        traverse(sbe_tree, result)
        return result

    return pd.DataFrame(list(df.explain.apply(flatten_sbe_stages).explode()))


def get_sbe_stage(stages_df: pd.DataFrame, stage_name: str) -> pd.DataFrame:
    """Filter the SBE stages DataFrame by the given SBE stage name."""
    return stages_df[stages_df.stage == stage_name].copy()


def extract_qsn_nodes(df: pd.DataFrame) -> pd.DataFrame:
    """Extract QSN Nodes and execution statistics from calibration DataFrame."""

    def extract(df_seq):
        es_dict = get_execution_stats(df_seq["classic"], df_seq["qsn"], [])

        rows = []
        for qsn_type, es in es_dict.items():
            for stat in es:
                row = {
                    "node_type": qsn_type,
                    **dataclasses.asdict(stat),
                    **json.loads(df_seq["query_parameters"]),
                    "run_id": df_seq.run_id,
                    "command": df_seq.command,
                    "source": df_seq.name,
                }
                rows.append(row)
        return rows

    return pd.DataFrame(list(df.apply(extract, axis=1).explode()))


def print_trees(calibration_df: pd.DataFrame, qsn_df: pd.DataFrame, row_index: int = 0):
    """Print classic and QSN Trees."""
    row = calibration_df.loc[qsn_df.iloc[row_index].source]
    print("CLASSIC")
    row.classic.print()
    print("\QSN")
    row.qsn.print()


def print_explain(calibration_df: pd.DataFrame, qsn_df: pd.DataFrame, row_index: int = 0):
    """Print explain."""
    row = calibration_df.loc[qsn_df.iloc[row_index].source]
    explain = json.loads(row.explain)
    explain_str = json.dumps(explain, indent=4)
    print(explain_str)


def calibrate(qsn_node_df: pd.DataFrame, variables: list[str] = None):
    """Calibrate the QSN node given in qsn_node_df with the given model input variables."""
    if variables is None:
        variables = ["n_processed"]
    y = qsn_node_df["execution_time"]
    X = qsn_node_df[variables]
    X = sm.add_constant(X)

    nnls = LinearRegression(positive=True, fit_intercept=False)
    model = nnls.fit(X, y)
    y_pred = model.predict(X)
    print(f"R2: {r2_score(y, y_pred)}")
    print(f"Coefficients: {model.coef_}")

    sns.scatterplot(x=qsn_node_df["n_processed"], y=qsn_node_df["execution_time"])
    sns.lineplot(x=qsn_node_df["n_processed"], y=y_pred, color="red")


if __name__ == "__main__":
    import asyncio

    from config import DatabaseConfig

    async def test():
        """Smoke tests."""
        database_config = DatabaseConfig(
            connection_string="mongodb://localhost",
            database_name="qsn_calibration",
            dump_path="~/mongo/buildscripts/cost_model/dump",
            restore_from_dump=True,
            dump_on_exit=False,
        )
        database = DatabaseInstance(database_config)

        raw_df = await load_calibration_data(database, "calibrationData")
        print(raw_df.head())

        cleaned_df = remove_outliers(raw_df, 0.0, 0.9)
        print(cleaned_df.head())

        qsn_nodes_df = extract_qsn_nodes(cleaned_df)
        print(qsn_nodes_df.head())

    loop = asyncio.get_event_loop()
    loop.run_until_complete(test())
