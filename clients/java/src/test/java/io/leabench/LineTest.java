/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import io.leabench.gen.BondingCreateOptions;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;

class LineTest {
    @Test
    void quoting() {
        assertEquals("abc", Line.quote("abc"));
        assertEquals("\"My Phone\"", Line.quote("My Phone"));
        assertEquals("\"a\\\"b\"", Line.quote("a\"b"));
        assertEquals("\"Zo\\xc3\\xab\"", Line.quote("Zoë"));
        assertEquals("1", Line.quote(true));
        assertEquals("\"\"", Line.quote(""));
    }

    @Test
    void build() {
        Map<String, Object> keys = new LinkedHashMap<>();
        keys.put("freq", 1000);
        keys.put("level", null);
        assertEquals("bcast tone 2 freq=1000", Line.build("bcast tone", List.of(2), keys));
    }

    @Test
    void enumChecked() {
        assertThrows(IllegalArgumentException.class, () -> new BondingCreateOptions().io("loud"));
    }

    @Test
    void json() {
        Map<String, Object> m = Json.object("{\"type\":\"evt\",\"t\":12,\"evt\":\"x\","
                + "\"name\":\"Hall \\\"A\\\"\",\"n\":\"Zoë\",\"a\":[1,2.5,true,null]}");
        assertEquals("evt", m.get("type"));
        assertEquals(12L, m.get("t"));
        assertEquals("Hall \"A\"", m.get("name"));
        assertEquals("Zoë", m.get("n"));
        assertEquals(List.of(1L, 2.5, true), ((List<?>) m.get("a")).subList(0, 3));
    }

    @Test
    void commandOf() {
        Map<String, String> wires = io.leabench.gen.Protocol.WIRES;
        assertEquals("broadcast bis add", Line.commandOf("broadcast 2 bis add freq=1", wires));
        assertEquals("broadcast start", Line.commandOf("broadcast 1 start", wires));
        assertEquals("broadcast start_all", Line.commandOf("broadcast start --all", wires));
        assertEquals("bonding pair", Line.commandOf("bonding pair Tel sec=4", wires));
    }
}
