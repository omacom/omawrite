# Issue tracker: Local Markdown

Issues and specs for this repo live as markdown files in `.scratch/`.

## Conventions

- One feature per directory: `.scratch/<feature-slug>/`
- The spec is `.scratch/<feature-slug>/spec.md`
- Implementation issues are one file per ticket at `.scratch/<feature-slug>/issues/<NN>-<slug>.md`, numbered from `01`
- Triage state is recorded as a `Status:` line near the top of each issue file (see `triage-labels.md`)
- Comments append under a `## Comments` heading

## When a skill says "publish to the issue tracker"

Create a file under `.scratch/<feature-slug>/`.

## When a skill says "fetch the relevant ticket"

Read the referenced local file. The user normally supplies its path or issue number.
