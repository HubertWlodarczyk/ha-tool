/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.hatool;

import java.util.Collections;
import java.util.Map;

/** A command answered ERR (or the shell rejected it). */
public class HaToolException extends RuntimeException {
    private static final long serialVersionUID = 1L;

    private final String command;
    private final int errno;
    private final String msg;
    private final Map<String, String> fields;

    /**
     * Creates the exception of a failed command.
     *
     * @param command the command line
     * @param errno negative errno of the ERR reply
     * @param msg message words of the ERR reply
     * @param fields key=value parts of the ERR reply
     */
    public HaToolException(String command, int errno, String msg, Map<String, String> fields) {
        super("'" + command + "' -> ERR " + errno + " " + msg + (fields.isEmpty() ? "" : " " + fields));
        this.command = command;
        this.errno = errno;
        this.msg = msg;
        this.fields = Collections.unmodifiableMap(fields);
    }

    /** The command line that failed */
    public String command() {
        return command;
    }

    /** Negative errno, e.g. -22 (invalid argument), -16 (busy: other mode), -7 (no air time) */
    public int errno() {
        return errno;
    }

    public String message() {
        return msg;
    }

    /** key=value parts of the error, e.g. key, cmd, allowed for an unknown key */
    public Map<String, String> fields() {
        return fields;
    }
}
