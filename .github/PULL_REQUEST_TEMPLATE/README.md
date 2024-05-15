# What Is This Folder

This folder is for custom pull request templates. Templates are Markdown (\*.md) files.

These custom templates can be used for example, by individual teams to have a custom pull request template with team specific testing or documentation instructions.

Read more in [Github's docs](https://docs.github.com/en/communities/using-templates-to-encourage-useful-issues-and-pull-requests/creating-a-pull-request-template-for-your-repository)

# How To Use This Folder

To create a custom template, create a new markdown file in this folder.

Then create a link of the form `https://github.com/10gen/mongo/compare/main...my-branch?quick_pull=1&template=your_new_template.md`

Share that link in your team docs to use for creating PRs. By selecting an unused values for `my-branch` it should show a branch selector when following the link.

Read more in [Github's docs](https://docs.github.com/en/pull-requests/collaborating-with-pull-requests/proposing-changes-to-your-work-with-pull-requests/using-query-parameters-to-create-a-pull-request)
