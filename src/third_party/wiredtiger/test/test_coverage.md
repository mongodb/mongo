|Component|Test Type|Testing Area|Description|Existing tests|
|---|---|---|---|---|
|Backup|Correctness|Full Backup|Full backup contains correct data|[../test/suite/test_backup01.py](../test/suite/test_backup01.py)
|Caching Eviction|Correctness|Written Data|Ensure that the data written out by eviction is correct after reading|[../test/suite/test_hs15.py](../test/suite/test_hs15.py)
|Checkpoints|Correctness|Checkpoint Data|On system with a complex, concurrent workload the correct versions of data appear in checkpoints|[../test/suite/test_checkpoint02.py](../test/suite/test_checkpoint02.py), [../test/suite/test_checkpoint03.py](../test/suite/test_checkpoint03.py)
|Checkpoints|Liveness|Liveness|Identify bugs and race conditions related to checkpoints that can cause deadlocks or livelocks|[../test/csuite/wt3363_checkpoint_op_races/main.c](../test/csuite/wt3363_checkpoint_op_races/main.c)
