# Distance expression test.

from __future__ import print_function
from builtins import range
from pymongo import MongoClient
import numpy as np
import random
import time
import string
from bson import binary


client = MongoClient()
db = client["test_speed"]

vec_size = 4096
num_documents = 1500
fill_data_base = True
iterations = 10
functions = ['no_op', 'cossim', 'chi2', 'euclidean', 'squared_euclidean', 'manhattan', 'no_op']

vec = []

for _ in range(iterations):
    vec.append(binary.Binary(np.random.rand(vec_size).astype(np.float32).tobytes()))


if fill_data_base:
    print("load database")
    db.test_speed.drop()
    for i in range(num_documents):
        if i % 1000 == 0: print(i)
        db.test_speed.insert({
            "id": random.randint(0, 1000000),
            "other_id": ''.join(np.random.choice(list(string.ascii_uppercase)) for _ in range(6)),
            "vector": binary.Binary(np.random.rand(vec_size).astype(np.float32).tobytes())
        })

    print("database loaded", db.test_speed.count())

times_aggregate_base = np.zeros([iterations, 1], dtype=np.float32)
for function in functions:
    for index in range(iterations):
        start = time.time()
        result = db.test_speed.aggregate([
            {   
                '$project':
                {
                    'id': '$id',
                    "other_id": '$other_id',
                    'distance': {'${}'.format(function): [vec[index], '$vector']},
                },
            },
            {"$sort": {"distance": -1}},
            {"$limit": 20}
        ])
        selection = list(result)
        times_aggregate_base[index] = time.time() - start

    print("Aggregate distance {}:".format(function))
    print("   - average: {:.5f}ms".format(np.mean(times_aggregate_base) * 1000))
    print("   - std:     {:.5f}ms".format(np.std(times_aggregate_base) * 1000))
    print("   - max:     {:.5f}ms".format(np.max(times_aggregate_base) * 1000))
    print("   - min:     {:.5f}ms".format(np.min(times_aggregate_base) * 1000))
    print("   - median:  {:.5f}ms".format(np.median(times_aggregate_base) * 1000))
