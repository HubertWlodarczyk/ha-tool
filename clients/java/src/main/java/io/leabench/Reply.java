/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import io.leabench.gen.Protocol;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Map;

/** Final OK of a command, with the data lines (DEV, BIS, ...) printed before it. */
public final class Reply extends Values {
    private final String command;
    private final List<Map<String, String>> data;

    Reply(String command, Map<String, String> values, List<Map<String, String>> data) {
        super(values, Protocol.REPLY_FIELDS.get(command));
        this.command = command;
        this.data = Collections.unmodifiableList(data);
    }

    public String command() {
        return command;
    }

    /** All data lines; each has "tag" (DEV, BIS, ...) and its fields */
    public List<Map<String, String>> data() {
        return data;
    }

    /** Data lines of one tag */
    public List<Values> rows(String tag) {
        List<Values> out = new ArrayList<>();
        for (Map<String, String> d : data) {
            if (tag.equals(d.get("tag"))) {
                out.add(new Values(d, null));
            }
        }
        return out;
    }
}
