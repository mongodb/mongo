# Resmoke suite execution via Bazel

Each variant runs its suites two ways, chosen **per suite** by the
`incompatible_with_bazel_remote_test` tag:

- **RBE-compatible** → remotely on the **EngFlow cluster**
- **incompatible** → locally on the **Evergreen host**

The two sets are disjoint; their union is the variant's selected suites.

## Where suites run

Edges: **solid** = `depends_on` · **dotted** = activation by the runner · **thick** = `bazel test` /
results.

```mermaid
flowchart LR
    subgraph EVG["Evergreen hosts"]
        ADT["archive_dist_test<br/>(compile variant)"]
        RUN["resmoke_tests runner"]
        RES["RBE result tasks<br/>(fetch results)"]
        subgraph LocalTasks["local tasks — 1 per incompatible suite"]
            L1["//suite-1<br/>bazel test --installed_dist_test"]
            L2["//suite-2"]
            L3["//…"]
        end
    end
    subgraph EF["EngFlow cluster"]
        SCH["scheduler"] --> WK["workers<br/>(run compatible suites)"]
    end

    RUN ==>|"bazel test --config=remote_test"| SCH
    WK ==>|"test outputs (logs)"| RES
    RUN -. "activate late<br/>(--build-events-file)" .-> RES

    RUN -. "activate early (--local-only)" .-> L1
    RUN -.-> L2
    RUN -.-> L3

    ADT -->|"depends_on"| L1
    ADT --> L2
    ADT --> L3

    classDef rbe fill:#dbeafe,stroke:#3b82f6;
    classDef loc fill:#fde68a,stroke:#d97706;
    classDef infra fill:#e5e7eb,stroke:#6b7280;
    class RUN,RES,SCH,WK rbe;
    class L1,L2,L3 loc;
    class ADT infra;
```

## Task generation

One `bazel_result_tasks_gen` runs
[generate_result_tasks.py](../../buildscripts/generate_result_tasks.py); per variant it emits both
sets.

```mermaid
flowchart TD
    G["bazel_result_tasks_gen<br/>→ generate_result_tasks.py"] --> V{"per variant"}
    V --> R["RBE set<br/>= ci-tags − incompatible"]
    V --> L["local set<br/>= ci-tags ∩ incompatible"]
    R --> RG["task GROUP resmoke_tests_results_&lt;variant&gt;<br/>result task / suite · activate:false"]
    L --> LT["STANDALONE task / suite<br/>tag resmoke_local_test · activate:false<br/>depends_on archive_dist_test"]

    classDef rbe fill:#dbeafe,stroke:#3b82f6;
    classDef loc fill:#fde68a,stroke:#d97706;
    class R,RG rbe;
    class L,LT loc;
```

## Execution

The runner starts the local tasks **early** so they run while the remote batch runs, then activates
the RBE result tasks **late** to fetch.

```mermaid
sequenceDiagram
    autonumber
    participant R as runner (Evergreen host)
    participant L as local tasks (Evergreen hosts)
    participant EF as EngFlow workers
    participant RBE as RBE result tasks

    R->>L: activate early (--local-only)
    par local — on Evergreen host
        L->>L: download dist-test + bazel test --installed_dist_test
    and remote — on EngFlow
        R->>EF: bazel test --config=remote_test
        EF-->>R: build_events.json
    end
    R->>RBE: activate late (--build-events-file)
    RBE->>RBE: fetch results
```

> Local tasks self-run, so they start early. RBE result tasks only fetch from the runner's
> `build_events.json`, so they must wait for it.
