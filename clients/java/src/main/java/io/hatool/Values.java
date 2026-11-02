/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.hatool;

import java.util.Collections;
import java.util.Map;

/** key=value fields of a reply, data line or event: strings, with typed getters. */
public class Values {
    protected final Map<String, String> values;
    private final Map<String, String> types;

    Values(Map<String, String> values, Map<String, String> types) {
        this.values = Collections.unmodifiableMap(values);
        this.types = types != null ? types : Collections.emptyMap();
    }

    /** All fields as strings */
    public Map<String, String> values() {
        return values;
    }

    /** True if the field is present */
    public boolean has(String key) {
        return values.containsKey(key);
    }

    /** Field value as text; IllegalArgumentException if absent */
    public String getString(String key) {
        String v = values.get(key);
        if (v == null) {
            throw new IllegalArgumentException("no field '" + key + "' in " + values);
        }
        return v;
    }

    /** Field value as int */
    public int getInt(String key) {
        return (int) getLong(key);
    }

    /** Field value as long (decimal or 0x hex) */
    public long getLong(String key) {
        String v = getString(key);
        return v.startsWith("0x") || v.startsWith("0X") ? Long.parseLong(v.substring(2), 16) : Long.parseLong(v);
    }

    /** Field value as double */
    public double getDouble(String key) {
        return Double.parseDouble(getString(key));
    }

    /** Field value as boolean (0 / no / off: false) */
    public boolean getBool(String key) {
        String v = getString(key);
        return !(v.equals("0") || v.isEmpty() || v.equals("no") || v.equals("off"));
    }

    /** Value converted by the protocol spec type (Long, Double, Boolean or String) */
    public Object get(String key) {
        String v = getString(key);
        try {
            switch (types.getOrDefault(key, "str")) {
                case "int": return getLong(key);
                case "hex": return Long.parseLong(v.replaceFirst("^0[xX]", ""), 16);
                case "float": return Double.parseDouble(v);
                case "bool": return getBool(key);
                default: return v;
            }
        } catch (NumberFormatException e) {
            return v;
        }
    }

    @Override
    public String toString() {
        return values.toString();
    }
}
