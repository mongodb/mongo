# Merging

### Standard Merge

If you have some changes locally that you want to merge into the mongodb repo, you must create a [pull request](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/creating-a-pull-request?tool=webui#creating-the-pull-request). Once the pull request is created you must get approval from an [owner](owners_format.md) of the files changed. If the files changed have no owner then you must get approval from one other engineer. Once you have gotten approval you will see a green merge button which, when pressed, will merge your code.

### Override / Emergency patch

> [!WARNING]  
> This is a distruptive action, please default to using [standard merges](#Standard-Merge).
>
> Some in-flight patches might fail, and that will make the authors sad.

An overriden patch will skip the merge queue and force all the pending merges in the queue to restart, since the base was updated.

![override_image](override_image.png)

#### Prerequisites

- [ ] The change **must** qualify as an [**emergency change**](#emergency-change).
- [ ] The change **must** have been approved by at least one [**overrider**](#overrider).
- [ ] The change **must** have been approved by at least one [owner](owners_format.md) of the affected modules.
- [ ] The change **must** have been re-synced (merge / rebase) with the targeted branch in the past 8h.
- [ ] The change **must** fully pass the "Commit Queue" check. This should be verified by the overrider.

> [!NOTE]
> Once all the prerequisites are met, the [**overrider**](#overrider) will then be able to initiate the emergency patch process by pressing the big red "Merge pull request" button.

#### Emergency Change

To qualify as an emergency change, it must fit into any of the following scenarios:

- Fixing the commit queue:
  - The [Standard Merge](#Standard-Merge) option is currently broken.
  - This change aims fix standard merge option.
- You are reverting a change that has caused a failure in mainline.
- You are making a large scale, time-sensitive changes. These are changes which could be very difficult to merge in normal conditions:
  - Renaming a prolific typo in the codebase.
  - Applying large-scale formatting.
  - Applying a generated fix.
  - Other...

#### Overrider

An overrider is defined as a member of [10gen/mongo-break-glass](https://mana.corp.mongodbgov.com/resources/664e9bed3d7d150379d3e0d0).

In most cases, module [owners](owners_format.md) should already be members of [10gen/mongo-break-glass](https://mana.corp.mongodbgov.com/resources/664e9bed3d7d150379d3e0d0).

## Merging into an older branch (backports)

TODO: in the interim please see the wiki for details
