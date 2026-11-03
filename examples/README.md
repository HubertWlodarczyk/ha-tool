# Examples

The examples use the Python client from `clients/python`. Install it from the
repository root:

```sh
python3 -m pip install -e clients/python
```

The complete generated command references are [CLI](../docs/cli.md),
[Python](../docs/python.md), and [Java](../docs/java.md).

## Simulation

Build the simulation and start a TCP endpoint:

```sh
scripts/build.sh sim
scripts/sim.py --tcp 7000
```

Run the Python example in another terminal:

```sh
python3 examples/python/quick_start.py
```

It creates and stops one small broadcast. To use a physical DK, replace
`LeaBench.tcp()` in the example with `LeaBench.open("/dev/ttyACM0")`.

## Java client

The [java/QuickStart.java](java/QuickStart.java) file uses the same scenario
and connects to `localhost:7000`:

```sh
cd clients/java
./gradlew build
```

Copy the file into a Java project that depends on the client and run it.
For a DK, replace `LeaBench.tcp("localhost", 7000)` with
`LeaBench.openSerial("/dev/ttyACM0")`.

## Raw CLI

The helper sends the same commands directly:

```sh
examples/cli/quick_start.sh /dev/ttyACM0
```

For a simulated pseudo-terminal, pass the path printed by
`scripts/sim.py --no-dut`:

```sh
examples/cli/quick_start.sh /dev/pts/12
```
