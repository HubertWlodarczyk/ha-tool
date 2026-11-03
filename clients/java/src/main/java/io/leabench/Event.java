/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import io.leabench.gen.Protocol;
import java.util.Map;

/** Asynchronous event (names: io.leabench.gen.Protocol.Events). */
public final class Event extends Values {
    private final String name;
    private final long tMs;

    Event(String name, long tMs, Map<String, String> values) {
        super(values, Protocol.EVENT_FIELDS.get(name));
        this.name = name;
        this.tMs = tMs;
    }

    public String name() {
        return name;
    }

    /** Board time of the event, ms since boot */
    public long timeMs() {
        return tMs;
    }

    @Override
    public String toString() {
        return name + " " + values;
    }
}
