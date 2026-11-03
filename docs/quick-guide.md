# leabench quick guide

This is the shortest path from a fresh checkout to a passing simulation test. The
full reference is in [README.md](../README.md); the CLI reference is generated in
[docs/protocol.md](protocol.md).

## 1. Prepare the workspace

Install the prerequisites for Zephyr and BabbleSim:

- Python 3.9 or newer,
- a working `west` installation (the Zephyr SDK is additionally needed for a
  physical DK build),
- a C compiler and the host tools required by BabbleSim,
- `pytest` and `ruff` for the Python tooling,
- a JDK 17 or newer if you are changing the Java client.

From the repository root, initialize the west workspace and fetch dependencies:

```sh
west init -l .
west update
west packages pip --install --ignore-venv-check
```

Apply the Zephyr patches required by this repository. The command is idempotent:

```sh
scripts/apply_patches.sh "$PWD/zephyr"
```

If Zephyr is not at `$ZEPHYR_BASE`, pass its path explicitly:

```sh
scripts/apply_patches.sh /path/to/zephyr
```

Build the BabbleSim host binaries. This is required before building or running
the simulation:

```sh
make -C tools/bsim everything BSIM_BUILD_FAIL_ASAP=1 -j"$(nproc)"
```

Set the workspace paths used by the simulation helpers:

```sh
export ZEPHYR_BASE="$PWD/zephyr"
export BSIM_OUT_PATH="$PWD/tools/bsim"
export BSIM_COMPONENTS_PATH="$PWD/tools/bsim/components"
```

## 2. Build and run the simulation

Build both the leabench firmware and the simulated device:

```sh
scripts/build.sh sim
```

Run the default test suite:

```sh
scripts/test.sh
```

Useful variants:

```sh
scripts/test.sh -k broadcast       # select tests by name
scripts/test.sh -m slow            # run only long fuzzing/soak tests
scripts/build.sh sim --sanitize    # ASan + UBSan build
scripts/build.sh sim --coverage    # coverage build
```

Simulation logs and binaries are written below `build/` and `tests/out/`. They
are local artifacts and are intentionally ignored by Git.

## 3. Try the interactive CLI

Start a TCP simulation:

```sh
scripts/sim.py --tcp 7000
```

In another terminal, connect with the Python client:

```python
from leabench import LeaBench

with LeaBench.tcp("localhost", 7000) as tool:
    print(tool.raw("version"))
```

For a complete client example, see [clients/python/README.md](../clients/python/README.md).

## 4. Work with a physical DK

Build the nRF5340 DK image and flash it using the normal Zephyr runner:

```sh
scripts/build.sh dk
west flash -d build/leabench_dk
```

The board exposes a USB VCOM port. The Python and Java clients accept a serial
device such as `/dev/ttyACM0` (or `COM3` on Windows). Run the same tests against
hardware with:

```sh
scripts/test.sh --hw-port /dev/ttyACM0
```

Use the hardware tests only with a board running the matching firmware; tests
that require the simulated device are skipped in this mode.

## 5. Before opening a pull request

Regenerate protocol outputs after changing `protocol/leabench.yaml`:

```sh
python3 tools/protogen/gen.py
```

Then run the repository checks:

```sh
scripts/lint.sh
```

If a generated file is reported as stale, do not edit it by hand. Regenerate it
from the protocol specification and include the generated changes in the same
pull request.

## 6. Firmware releases

Stable firmware releases are created from semantic-version tags:

```sh
git tag -a v0.3.0 -m "Release v0.3.0"
git push origin v0.3.0
```

GitHub Actions builds the DK sysbuild, merges the application and network-core
images, and attaches one whole-board `.hex` and one whole-board `.bin` to the
release. Do not upload the individual `leabench/` or `hci_ipc/` images as
release files.
