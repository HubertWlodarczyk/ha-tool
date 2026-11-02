/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.hatool;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Minimal JSON parser for the board's JSON Lines (objects, arrays, strings, numbers, literals). */
final class Json {
    private final String s;
    private int i;

    private Json(String s) {
        this.s = s;
    }

    static Object parse(String text) {
        Json j = new Json(text);
        j.ws();
        Object v = j.value();
        j.ws();
        if (j.i != j.s.length()) {
            throw j.error("trailing characters");
        }
        return v;
    }

    @SuppressWarnings("unchecked")
    static Map<String, Object> object(String text) {
        Object v = parse(text);
        if (!(v instanceof Map)) {
            throw new IllegalArgumentException("not a JSON object: " + text);
        }
        return (Map<String, Object>) v;
    }

    private IllegalArgumentException error(String what) {
        return new IllegalArgumentException("JSON " + what + " at " + i + ": " + s);
    }

    private void ws() {
        while (i < s.length() && Character.isWhitespace(s.charAt(i))) {
            i++;
        }
    }

    private Object value() {
        if (i >= s.length()) {
            throw error("unexpected end");
        }
        char c = s.charAt(i);
        if (c == '{') {
            return obj();
        } else if (c == '[') {
            return arr();
        } else if (c == '"') {
            return str();
        } else if (s.startsWith("true", i)) {
            i += 4;
            return Boolean.TRUE;
        } else if (s.startsWith("false", i)) {
            i += 5;
            return Boolean.FALSE;
        } else if (s.startsWith("null", i)) {
            i += 4;
            return null;
        }
        return num();
    }

    private Map<String, Object> obj() {
        Map<String, Object> m = new LinkedHashMap<>();
        i++;
        ws();
        if (peek('}')) {
            i++;
            return m;
        }
        while (true) {
            ws();
            String k = str();
            ws();
            expect(':');
            ws();
            m.put(k, value());
            ws();
            if (peek(',')) {
                i++;
            } else {
                expect('}');
                return m;
            }
        }
    }

    private List<Object> arr() {
        List<Object> l = new ArrayList<>();
        i++;
        ws();
        if (peek(']')) {
            i++;
            return l;
        }
        while (true) {
            ws();
            l.add(value());
            ws();
            if (peek(',')) {
                i++;
            } else {
                expect(']');
                return l;
            }
        }
    }

    private String str() {
        expect('"');
        StringBuilder b = new StringBuilder();
        while (i < s.length()) {
            char c = s.charAt(i++);
            if (c == '"') {
                return b.toString();
            }
            if (c == '\\') {
                char e = s.charAt(i++);
                if (e == 'u') {
                    b.append((char) Integer.parseInt(s.substring(i, i + 4), 16));
                    i += 4;
                } else {
                    b.append(unescape(e));
                }
            } else {
                b.append(c);
            }
        }
        throw error("unterminated string");
    }

    private static char unescape(char e) {
        switch (e) {
            case 'n':
                return '\n';
            case 't':
                return '\t';
            case 'r':
                return '\r';
            case 'b':
                return '\b';
            case 'f':
                return '\f';
            default:
                return e; // " \\ /
        }
    }

    private Object num() {
        int start = i;
        while (i < s.length() && "+-0123456789.eE".indexOf(s.charAt(i)) >= 0) {
            i++;
        }
        String t = s.substring(start, i);
        if (t.isEmpty()) {
            throw error("unexpected character");
        }
        if (t.contains(".") || t.contains("e") || t.contains("E")) {
            return Double.parseDouble(t);
        }
        return Long.parseLong(t);
    }

    private boolean peek(char c) {
        return i < s.length() && s.charAt(i) == c;
    }

    private void expect(char c) {
        if (!peek(c)) {
            throw error("expected '" + c + "'");
        }
        i++;
    }
}
