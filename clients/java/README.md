# leabench (Java)

`leabench` client for Java 17+. The only runtime dependency is jSerialComm (serial port).

```
cd clients/java
./gradlew build                 # jar in build/libs/, unit tests
./gradlew publishToMavenLocal   # io.leabench:leabench:0.2.0 into ~/.m2
```

```java
import io.leabench.*;
import io.leabench.gen.*;
import java.time.Duration;

try (LeaBench tool = LeaBench.openSerial("/dev/ttyACM0")) {      // "COM3"; LeaBench.find() locates the DK itself
    Reply d = tool.bonding().create(new BondingCreateOptions().name("Phone of José"));
    int id = d.getInt("id");
    tool.bonding().connect("Phone of José", new BondingConnectOptions().address("C0:11:22:33:44:55/random"));
    tool.waitEvent(Protocol.Events.BONDING_CONNECTED, Duration.ofSeconds(20), "id", id);
    tool.bonding().pair("Phone of José");
    tool.waitEvent(Protocol.Events.BONDING_PAIRED, Duration.ofSeconds(20), "id", id, "bonded", true);

    tool.mode("broadcast");                                   // clears the unicast context
    tool.broadcast().create(1, new BroadcastCreateOptions().name("Hall").preset("24_2").bis(2).code("Secret"));
    Reply t = tool.broadcast().bis().tone(1, 1, new BroadcastBisToneOptions().freq(1000).level(-20));
    double hz = t.getDouble("freq_actual");
    tool.broadcast().start(1);
} catch (LeaBenchException e) {
    System.err.println(e.errno() + " " + e.message() + " " + e.fields());
}
```

- **Commands:** every CLI command has its own method: `tool.broadcast().bis().add(1, ...)`,
  `tool.leSource().call().incoming(...)`, `tool.broadcast().startAll()` (`broadcast start --all`).
  - Required arguments are method parameters.
  - Optional arguments go through a `<Command>Options` class with setters.
  - The command code is generated from `protocol/leabench.yaml` into `io.leabench.gen` (do not edit
    by hand). All commands are described in `docs/protocol.md`.
  - A bad enum value (e.g. `io("loud")`) throws `IllegalArgumentException` before anything is
    sent.
- **Reply (`Reply`):** `getInt`, `getString`, `getDouble`, `getBool`, and `get(k)` typed
  according to the specification. Data lines come from `rows("IDENTITY")`.
- **Errors:** an `ERR` reply throws `LeaBenchException` with `errno()`, `message()` and `fields()`.
- **Events:** there are three ways:
  - `waitEvent(name, timeout, key, value, ...)`; by default events since the last command
    count, the variant with `since` takes a position from `mark()`,
  - `subscribe()` returns a `BlockingQueue`,
  - `addListener(...)`.
- **Threads:** the class is thread-safe, commands go one at a time.
- **Simulation without a board:** run `scripts/sim.py --tcp 7000`, then
  `LeaBench.tcp("localhost", 7000)`. The `SimulationTest` runs with
  `HATOOL_TCP=localhost:7000 ./gradlew test`.
- **Raw commands:** `tool.raw("broadcast 2 show")` sends any line.
