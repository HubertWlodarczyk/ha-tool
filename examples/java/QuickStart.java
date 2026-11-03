/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

import io.leabench.LeaBench;
import io.leabench.gen.BroadcastCreateOptions;
import io.leabench.gen.BroadcastBisToneOptions;
import java.time.Duration;

/** Create a small broadcast using the Java client. */
public final class QuickStart {
    private QuickStart() {}

    public static void main(String[] args) {
        try (LeaBench tool = LeaBench.tcp("localhost", 7000)) {
            tool.mode("broadcast");
            tool.broadcast().create(
                    1, new BroadcastCreateOptions().name("Example").preset("24_2").bis(1));
            tool.broadcast().bis().tone(
                    1, 1, new BroadcastBisToneOptions().freq(1000));
            tool.broadcast().start(1);
            System.out.println(tool.waitEvent("broadcast_started", Duration.ofSeconds(15), "big", 1));
            tool.broadcast().stop(1);
        }
    }
}
