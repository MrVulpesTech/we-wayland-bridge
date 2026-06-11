# Writing style

Binding rules for every piece of prose and code that lands in this
repository: documentation, comments, commit messages, session logs. The
goal is a single voice — technical, calm, distinguishable from
AI-generated filler at a glance.

These rules are enforced in review. They apply to project-authored text.
They do **not** apply to vendored text we did not write: the upstream
submodule under `upstream/`, the FSF GPL licence body, or quoted output
from other tools.

## Language and tone

### 1. English only

All committed prose — documentation, code comments, commit messages,
session logs, error strings — is written in English. Discussion in
issues, chat, and pull-request descriptions may be any language;
artefacts that ship in the repository may not.

The project targets a public GitHub repository and a pull request into
`Almamu/linux-wallpaperengine`, whose working language is English.

### 2. No emoji

Emoji do not appear in markdown files, code comments, or commit
messages. The exception is when an emoji is the *subject* of the text.
Status indicators use words (`pass`, `fail`, `pending`) or text symbols
(`✓`, `✗`).

### 3. No marketing voice, no AI filler

The following words and phrases are forbidden in prose. They mark text
as marketing-speak or LLM filler:

```text
robust          leverage        seamlessly       comprehensive
elegant         simply          essentially      basically
obviously       just (filler)   very             really
in conclusion   it is important to note
unleash         supercharge     game-changing    revolutionary
```

If a sentence still says what it needs to say after deleting one of
these words, the word was filler. Delete it. If the sentence collapses
without the word, the sentence was empty. Delete the sentence.

### 4. Imperative mood for instructions

Documentation that tells the reader to do something uses imperative:
"Run `cmake --build build`", not "You should run" or "It is recommended
to run". Same for commit subject lines: `Add PipeWire output backend`,
not `Added` or `Adding`.

## Content rules

### 5. No personal data

Names, email addresses, phone numbers, and other PII do not appear in
documentation, code, or fixtures — except where a person is named in a
legal capacity (a LICENSE or NOTICE file) with their consent. Examples
use fictional values: `alice`, `example.com`, `192.0.2.1`.

This project reads files from the operator's own Steam library. Never
copy, commit, or quote the content of those wallpapers (assets,
`scene.json`, shader source) into the repository. Paths and metadata
field names are fine; payloads are not. See `10_vision/10.03_non_goals.md`.

### 6. No invented numbers

Numbers in prose are either measured (with the method referenced) or
marked as a projection. Never write "60 % less CPU" or "runs at 4K@60"
without the measurement. Use one of:

- "Measured at X (see `docs/session-NN-*.md`)."
- "Target: X; not yet validated."
- Delete the number.

Frame rates, latency, and copy-vs-zero-copy claims are the whole
technical risk of this project. Treat every performance figure as a
claim that must be backed by a session log before it ships as fact.

### 7. Comments explain why, not what

Code comments justify decisions, point at constraints, or warn future
readers. They do not narrate code. The code is the *what*.

Good:

```text
// Mutter rejects wlr-layer-shell, so the actor must live in the
// background layer of the shell process itself — see ADR-0001.
```

Bad:

```text
// Add the actor to the background group.
```

## File hygiene

### 8. One concept, one file

If something does not work, fix the existing file. Do not create
`backend_v2.cpp`, `extension_new.js`, or `output-backends-final.md`.
Git remembers the previous attempt. The same applies to documentation:
if `30.01_output_backends.md` is wrong, edit it.

### 9. Do not edit `upstream/`

`upstream/` is a git submodule pinned to a specific upstream commit. Do
not edit files inside it. Renderer changes live as patches or a tracked
fork branch per ADR-0001 and ADR-0003. Editing the submodule in place
produces a dirty submodule that cannot be cleanly upstreamed.

### 10. Numbering is stable

Once a documentation section has a Johnny.Decimal number (`20.03`,
`ADR-0004`), that number does not change across edits. New sections take
the next free number, even if it leaves gaps. References from other
documents must keep working.

## Markdown style

### 11. Tables for tabular data

If the content is "name, type, value, default" — that is a table, not a
bulleted list with embedded colons.

### 12. Code blocks have a language

Every fenced code block specifies its language: `sh`, `cpp`, `js`,
`json`, `cmake`, `text`. For directory trees, sample output, and ASCII
diagrams use `text`. Plain unlabelled fences are forbidden.

### 13. Restraint with emphasis

`**bold**` for the single most important phrase per paragraph, at most.
No `***bold italic***`. No ALL-CAPS for emphasis (acronyms are fine:
GPU, IPC, dma-buf).

## Commits

Commit subject: imperative, under 70 characters, no trailing period.
The body explains *why*; the diff already shows *what*. Reference ADRs
or session logs by stable identifier where applicable.

```text
Add headless render target to the output backend

Renders the scene into an offscreen FBO instead of a window so the
frames can be handed to PipeWire (ADR-0003). No window backend code
changed; this is a third sibling under Render/Drivers/Output.
```

## Audit

When in doubt about a rule, search the repository for prior application:

```sh
git grep -iE "robust|leverage|seamlessly|essentially|obviously"
```

If a match is found in project-authored text, fix it as part of the PR
you are already preparing. Style fixes are not a separate workstream.
