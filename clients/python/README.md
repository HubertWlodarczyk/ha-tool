# leabench (Python)

`leabench` client for Python ≥ 3.9. The only dependency is `pyserial`.

```
pip install -e clients/python        # from the repository
```

```python
from leabench import LeaBench, LeaBenchError

with LeaBench.open(
    "/dev/ttyACM0"
) as tool:  # Windows: "COM3"; LeaBench.find() locates the DK itself
    d = tool.bonding.create("My phone")  # values with spaces are fine
    tool.bonding.connect("My phone", address="C0:11:22:33:44:55/random")
    tool.wait_event("bonding_connected", id=d.id, timeout=20)
    tool.bonding.pair(d.id)
    tool.wait_event("bonding_paired", id=d.id, bonded=1, timeout=20)

    tool.mode("broadcast")  # clears the unicast context
    tool.broadcast.create(1, name="Hall", preset="24_2", bis=2, code="Secret")
    tool.broadcast.bis.tone(1, 1, freq=1000, level=-20)
    tool.broadcast.start(1)
    for ev in tool.subscribe(timeout=60):  # events from now on (TimeoutError after 60 s of silence)
        print(ev.name, ev.values)
```

- **Commands:** every CLI command has its own method (`tool.broadcast.bis.add(1, ...)`,
  `tool.le_source.call.incoming(...)`, `tool.broadcast.start_all()`), generated from
  `protocol/leabench.yaml`. All commands are described in `docs/protocol.md`. A typo in an argument
  name raises `TypeError` before anything is sent, and a bad enum value raises `ValueError`.
- **Reply (`Reply`):** `r.id` returns the value converted according to the specification (int,
  float, bool), `r["id"]` the raw string. Data lines come from `r.rows("IDENTITY")`.
- **Errors:** an `ERR` reply raises `LeaBenchError` with the fields `errno`, `msg` and `fields`.
- **Events:** there are three ways:
  - `wait_event(name, timeout, **match)`; by default events since the last command count,
    `since=tool.mark()` sets a different starting point,
  - `subscribe()`,
  - `on_event(callback)`.
- **Simulation without a board:** run `scripts/sim.py --tcp 7000`, then
  `LeaBench.tcp("localhost", 7000)`.
- **Raw commands:** `tool.raw("broadcast 2 show")` sends any line and returns a `Reply`.
