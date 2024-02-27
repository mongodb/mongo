# Evergreen Task Generation

Generating tasks is a way to dynamically create tasks in Evergreen builds. This is done via the
['generate.tasks'](https://github.com/evergreen-ci/evergreen/wiki/Project-Commands#generatetasks)
evergreen command.

Task generation allow us to do things like dynamically split a task into sub-tasks that can be run
in parallel, or generate sub-tasks to run against different mongodb versions.

Task generation is typically done with the [mongo-task-generator](https://github.com/mongodb/mongo-task-generator)
tool. Refer to its [documentation](https://github.com/mongodb/mongo-task-generator/blob/master/docs/generating_tasks.md)
for details on how it works.

## Configuring a task to be generated

In order to generate a task, we typically create a placeholder task. By convention the name of
these tasks should end in "\_gen". Most of the time, generated tasks should inherit the
[gen_task_template](https://github.com/mongodb/mongo/blob/31864e3866ce9cc54c08463019846ded2ad9e6e5/etc/evergreen_yml_components/definitions.yml#L99-L107)
which configures the required dependencies.

The placeholder tasks needs to have the "generate resmoke tasks" function as one of its `commands`.
This is how the `mongo-task-generator` knows that the task needs to be generated. You can also
add `vars` to the function call to configure how the task will generated. You can refer to
the [mongo-task-generator](https://github.com/mongodb/mongo-task-generator/blob/master/docs/generating_tasks.md#use-cases)
documentation for details on what options are available.

Once a placeholder task in defined, you can reference it just like a normal task.

## The task generation process

Task generation is performed as a 2-step process.

1. The first step is to generator the configuration for the generated tasks and send that to
   evergreen to actually create the tasks. This is done by the `version_gen` task using the
   `mongo-task-generator` tool. This only needs to be done once for the entire version and rerunning
   this task will result in a no-op.

    The tasks will be generated in an "inactive" state. This allows us to generate all available
    tasks, regardless of whether they are meant to be run or not. This way if we choose to run
    additional tasks in the future, they will exist to be run.

    This step will also hide all the placeholder tasks into a display task called `generator_tasks`
    in each build variant. Once task generation is completed, the user should perform actions on
    the generated tasks instead of the placeholder tasks, we encourage this by hiding the
    placeholder tasks from view.

2. After the tasks have been generated, the placeholder tasks are free to run. The placeholder tasks
   simply find the task generated for them and mark it activated. Since generated tasks are
   created in the "inactive" state, this will activate any generated tasks whose placeholder task
   runs. This enables users to select tasks to run on the initial task selection page even though
   the tasks have not yet been generated.

**Note**: While this 2-step process allows a similar user experience to working with normal tasks,
it does create a few UI quirks. For example, evergreen will hide "inactive" tasks in the UI, as a
result, a task might disappear for some time after being created, until the placeholder task runs
and activates it.
