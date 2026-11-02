/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.hatool;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import io.hatool.gen.BroadcastBisAddOptions;
import io.hatool.gen.BroadcastCreateOptions;
import io.hatool.gen.BroadcastSubgroupAddOptions;
import io.hatool.gen.BondingCreateOptions;
import io.hatool.gen.Protocol;
import java.time.Duration;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;

/** Against a simulated board: HATOOL_TCP=localhost:7000 (scripts/sim.py --tcp 7000). */
class SimulationTest {
    private static HaTool open() {
        String target = System.getenv("HATOOL_TCP");
        assumeTrue(target != null && !target.isEmpty(), "HATOOL_TCP not set");
        String[] hp = target.split(":");
        return HaTool.tcp(hp[0], Integer.parseInt(hp[1]), System.out::println);
    }

    @Test
    void scenario() throws Exception {
        try (HaTool tool = open()) {
            tool.reset();
            assertEquals(Protocol.VERSION, tool.version().getInt("proto"));
            assertEquals(16, tool.limits().rows("LC3").size());

            // Identity with a non-ASCII name
            Reply d = tool.bonding().create(new BondingCreateOptions().name("Phone of José").io("yesno"));
            assertEquals("Phone of José", d.getString("name"));
            assertEquals(1L, d.get("id"));
            assertEquals("Phone of José", tool.bonding().list().rows("IDENTITY").get(0).getString("name"));

            // Errors: unknown key (board), wrong mode
            HaToolException e = assertThrows(HaToolException.class, () -> tool.raw("bonding create frq=1"));
            assertEquals(-22, e.errno());
            assertEquals("frq", e.fields().get("key"));
            e = assertThrows(HaToolException.class, () -> tool.broadcast().create(1));
            assertEquals(-16, e.errno()); // unicast mode active

            // Broadcast built step by step
            tool.mode("broadcast");
            tool.broadcast().create(1, new BroadcastCreateOptions().name("Java hall").rtn(1));
            tool.broadcast().subgroup().add(1, new BroadcastSubgroupAddOptions().preset("48_2").lang("pol"));
            Reply bis = tool.broadcast().bis().add(1, new BroadcastBisAddOptions().freq(1013).level(-20));
            assertEquals(1000.0, bis.getDouble("freq_actual"));
            assertEquals("48_2", bis.getString("cfg"));
            BlockingQueue<Event> q = tool.subscribe();
            tool.broadcast().start(1);
            Event started = tool.waitEvent(Protocol.Events.BROADCAST_STARTED, Duration.ofSeconds(15), "big", 1);
            assertEquals("Java hall", started.getString("name"));
            Event fromQueue = q.poll(5, TimeUnit.SECONDS);
            assertTrue(fromQueue != null);
            tool.unsubscribe(q);
            tool.reset();
        }
    }
}
