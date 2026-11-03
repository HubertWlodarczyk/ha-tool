/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Map;
import java.util.function.Consumer;

/**
 * Reader thread of a transport: splits the byte stream into UTF-8 lines and hands each one on as a
 * JSON object (Map) or, if it is not JSON (shell errors, echo), as a String.
 */
final class LineReader implements AutoCloseable {
    private static final int POLL_MS = 100;

    private final Transport transport;
    private final Consumer<String> log;
    private final Consumer<Object> sink;
    private final Thread thread;
    private volatile boolean stop;

    LineReader(Transport transport, Consumer<String> log, Consumer<Object> sink) {
        this.transport = transport;
        this.log = log;
        this.sink = sink;
        thread = new Thread(this::run, "leabench-rx");
        thread.setDaemon(true);
    }

    void start() {
        thread.start();
    }

    private void run() {
        byte[] partial = new byte[0];
        while (!stop) {
            byte[] chunk;
            try {
                chunk = transport.read(POLL_MS);
            } catch (IOException e) {
                break;
            }
            if (chunk.length > 0) {
                partial = split(concat(partial, chunk));
            }
        }
    }

    /** Hands on every complete line of buf, returns the incomplete rest */
    private byte[] split(byte[] buf) {
        int start = 0;
        for (int i = 0; i < buf.length; i++) {
            if (buf[i] == '\n') {
                String line = new String(buf, start, i - start, StandardCharsets.UTF_8)
                        .replace("\r", "")
                        .replace("\0", "")
                        .trim();
                if (!line.isEmpty()) {
                    deliver(line);
                }
                start = i + 1;
            }
        }
        return Arrays.copyOfRange(buf, start, buf.length);
    }

    private void deliver(String line) {
        if (log != null) {
            log.accept("<< " + line);
        }
        Object obj = line;
        if (line.startsWith("{")) {
            try {
                Map<String, Object> m = Json.object(line);
                obj = m;
            } catch (IllegalArgumentException e) {
                obj = line;
            }
        }
        sink.accept(obj);
    }

    private static byte[] concat(byte[] a, byte[] b) {
        byte[] out = Arrays.copyOf(a, a.length + b.length);
        System.arraycopy(b, 0, out, a.length, b.length);
        return out;
    }

    @Override
    public void close() {
        stop = true;
        try {
            thread.join(2L * POLL_MS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
