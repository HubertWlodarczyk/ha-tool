#!/usr/bin/env python3
# Copyright (c) 2026 Hubert Włodarczyk
# SPDX-License-Identifier: MPL-2.0

"""Create a small broadcast in the simulator."""
from leabench import LeaBench


def main() -> None:
    with LeaBench.tcp() as tool:
        tool.mode("broadcast")
        tool.broadcast.create(1, name="Example", preset="24_2", bis=1)
        tool.broadcast.bis.tone(1, 1, freq=1000)
        events = tool.subscribe(timeout=15)
        tool.broadcast.start(1)
        print(next(events))
        tool.broadcast.stop(1)


if __name__ == "__main__":
    main()
