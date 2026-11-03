/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;

/** Command line building: values quoted when needed, non-ASCII as UTF-8 \xHH escapes. */
public final class Line {
    private Line() {}

    /** One argument as the shell expects it */
    public static String quote(Object value) {
        if (value instanceof Boolean) {
            return (Boolean) value ? "1" : "0";
        }
        String s = String.valueOf(value);
        boolean plain = !s.isEmpty();
        for (int i = 0; i < s.length() && plain; i++) {
            char c = s.charAt(i);
            plain = c > 0x20 && c < 0x7f && c != '"' && c != '\\';
        }
        if (plain) {
            return s;
        }
        StringBuilder b = new StringBuilder("\"");
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            i += Character.charCount(cp);
            if (cp == '"' || cp == '\\') {
                b.append('\\').append((char) cp);
            } else if (cp < 0x80) {
                b.append((char) cp);
            } else {
                for (byte x : new String(Character.toChars(cp)).getBytes(StandardCharsets.UTF_8)) {
                    b.append(String.format("\\x%02x", x & 0xff));
                }
            }
        }
        return b.append('"').toString();
    }

    /** 'bcast bis add' + positional arguments + key=value (null values left out) */
    public static String build(String command, List<Object> args, Map<String, Object> keys) {
        StringBuilder b = new StringBuilder(command);
        for (Object a : args) {
            if (a != null) {
                b.append(' ').append(quote(a));
            }
        }
        for (Map.Entry<String, Object> e : keys.entrySet()) {
            if (e.getKey().startsWith("--")) {
                if (Boolean.TRUE.equals(e.getValue())) {
                    b.append(' ').append(e.getKey()); // flag
                }
            } else if (e.getValue() != null) {
                b.append(' ').append(e.getKey()).append('=').append(quote(e.getValue()));
            }
        }
        return b.toString();
    }

    /**
     * Command of the specification a line runs ("broadcast 2 bis add ..." is "broadcast bis add"):
     * the longest command line pattern that matches, a {target} word matching any value.
     */
    static String commandOf(String line, Map<String, String> wires) {
        String[] w = line.trim().split("\\s+");
        String best = w.length > 0 ? w[0] : "";
        int bestLen = 0;
        int bestTargets = 0;
        for (Map.Entry<String, String> e : wires.entrySet()) {
            String[] p = e.getValue().split(" ");
            if (p.length > w.length) {
                continue;
            }
            boolean ok = true;
            int targets = 0;
            for (int i = 0; i < p.length && ok; i++) {
                if (p[i].startsWith("{")) {
                    targets++;
                    ok = !w[i].contains("=") && !w[i].startsWith("--");
                } else {
                    ok = p[i].equals(w[i]);
                }
            }
            if (ok && (p.length > bestLen || (p.length == bestLen && targets < bestTargets))) {
                best = e.getKey();
                bestLen = p.length;
                bestTargets = targets;
            }
        }
        return best;
    }
}
