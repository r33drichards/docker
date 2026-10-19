# Maintain and publish the documentation

Use Python 3.11+ in a virtual environment, from the repository root:

~~~sh
python3 -m venv .venv-docs
.venv-docs/bin/pip install -r requirements-docs.txt
python3 tools/generate_docs_reference.py
.venv-docs/bin/mkdocs build --strict
.venv-docs/bin/mkdocs serve --dev-addr 127.0.0.1:8000
~~~

Open http://127.0.0.1:8000/docker/ (or the URL printed by MkDocs). Stop with Ctrl-C.
`site/` and the virtual environment are ignored by Git. Reference excerpts and
hook/type assets are generated from `JAVASCRIPT.md`, the reviewed hook inventory
and TypeScript declarations. Edit those sources then regenerate; `--check` detects drift.

Put learning steps in tutorials, task procedures in how-to guides, exact behavior
in reference, and design rationale in explanation. Do not turn a lesson into a
configuration catalogue or hide safety limits only in an explanatory essay.

The documentation workflow builds on PRs without publishing. On pushes to master
or explicit workflow dispatch it may publish to `gh-pages` using the repository
workflow token. GitHub Pages must use the gh-pages branch root. Dispatch requires
the workflow to exist on the default branch; do not merge the module PR merely
to publish docs. An administrator can publish a reviewed checkout manually:

~~~sh
.venv-docs/bin/mkdocs gh-deploy --strict --message 'Publish JavaScript documentation'
~~~

Use an ephemeral approved Git credential, not a token in a URL or committed file.
This command changes the documentation branch only. Site publication is not an
IRC daemon/module deployment, and a successful build alone does not prove Pages is live.
