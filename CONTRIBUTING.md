# Working with the code

## Style

| Language | Style | Tool |
|---|---|---|
| C (firmware, LC3 generator) | Zephyr (tabs, 100 columns) | `.clang-format`, Zephyr checkpatch (`.checkpatch.conf`) |
| Python (tests, scripts, tools, `clients/python`) | PEP 8, 100 columns | ruff (`pyproject.toml`) |
| Java (`clients/java`) | 4 spaces, 120 columns | checkstyle (`clients/java/config/checkstyle`), `javac -Xlint:all -Werror` |

- Every source file starts with a `Copyright` + `SPDX-License-Identifier: MPL-2.0` header.
- Code, comments, messages and documentation are in English.
- A comment explains *why* (decisions, controller limitations, measurements), not *what*.
- One module, one responsibility; shell commands are kept apart from the logic
  (`*_cmds.c` / `cmds.c`).
- A command error: `return cli_err(sh, -EINVAL, "...")`, or `cli_err_hint(...)` when there is
  an obvious next step; replies and events use `key=value`.

## Checks

```
scripts/lint.sh          # clang-format, checkpatch, headers, ruff, generated files, gradle check
scripts/lint.sh --fix    # automatic fixes first (clang-format, ruff)
scripts/analyze.sh       # gcc -fanalyzer and clang-tidy (needs the simulation builds)
scripts/build.sh sim     # simulation build
scripts/test.sh          # tests in BabbleSim
scripts/build.sh dk      # nRF5340 DK build (memory of both cores in the report)
```

Before a commit, lint must be clean and the tests green.

## Adding or changing a CLI command

1. **Specification:** describe it in `protocol/leabench.yaml` (line form, arguments, keys with
   types, flags, reply fields, data lines, new events).
2. **Generation:** `tools/protogen/gen.py` regenerates the firmware command table
   (`leabench/src/cli_table.c`, `cli_cmds.h`), the Python and Java clients and
   `docs/protocol.md`.
3. **Firmware handler:** implement `cmd_<path words>` declared in `cli_cmds.h`. The dispatcher
   (`common/src/cli_tree.c`) has already checked the positional arguments, keys and flags against
   the specification; print user-supplied text values through `CLI_Q()` / `CLI_QS()`.
4. **Tests:** a test in `tests/`. The conformance report checks every reply and event against
   the specification.

## LC3 frame table

`scripts/gen_lc3_frames.sh` regenerates `leabench/src/lc3_frames.c` (deterministically, with a
decoding self-test). The file is never edited by hand.

## Zephyr patches

The patches live in `zephyr/patches/zephyr/` and are listed in `zephyr/patches.yml` (with their
sha256). `west patch apply` or `scripts/apply_patches.sh` applies them. A new patch is made with
`git diff` in the Zephyr workspace and must apply in order on a clean Zephyr.
