/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import com.fazecast.jSerialComm.SerialPort;
import io.leabench.gen.Protocol;
import io.leabench.gen.RootCommands;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.function.Consumer;
import java.util.function.Predicate;

/**
 * One leabench board (nRF5340 DK, or a simulated board over TCP).
 *
 * <pre>{@code
 * try (LeaBench tool = LeaBench.openSerial("/dev/ttyACM0")) {   // LeaBench.find(), LeaBench.tcp(host, port)
 *     Reply d = tool.bonding().create(new BondingCreateOptions().name("Phone"));
 *     tool.bonding().connect("Phone", new BondingConnectOptions().address("C0:11:22:33:44:55/random"));
 *     Event e = tool.waitEvent("bonding_connected", Duration.ofSeconds(20), "id", d.getInt("id"));
 * }
 * }</pre>
 *
 * <p>Every CLI command is a method (generated from protocol/leabench.yaml, see docs/protocol.md).
 * ERR replies throw {@link LeaBenchException}. Thread safe: commands are sent one at a time.
 */
public final class LeaBench extends RootCommands implements AutoCloseable {
    /** Baud rate of the DK's VCOM */
    public static final int DEFAULT_BAUD = 1_000_000;
    /** USB vendor id of the J-Link OB of the nRF5340 DK */
    public static final int SEGGER_VID = 0x1366;

    private static final Duration REPLY_TIMEOUT = Duration.ofSeconds(10);
    private static final Duration SYNC_TIMEOUT = Duration.ofSeconds(20);
    private static final Duration FIND_TIMEOUT = Duration.ofSeconds(2);
    private static final int EINVAL = -22;
    private static final String[] SHELL_ERRORS = {
        "command not found", "wrong parameter count", "specify a subcommand",
        "non-ASCII character", "not terminated", "Too many arguments",
    };

    private final Transport transport;
    private final Consumer<String> log;
    private final LineReader reader;
    private final EventLog events = new EventLog();
    private final List<Object> replies = new ArrayList<>(); // Map (JSON line) or String (text)
    private int mark;

    /**
     * Connects over a transport, switches the board to JSON Lines and checks the protocol version.
     *
     * @param transport serial port or TCP connection
     * @param log receives every line sent (">> ") and received ("<< "), may be null
     * @param syncTimeout how long to wait for the board to answer
     */
    public LeaBench(Transport transport, Consumer<String> log, Duration syncTimeout) {
        this.transport = transport;
        this.log = log;
        reader = new LineReader(transport, log, this::onLine);
        reader.start();
        try {
            sync(syncTimeout);
        } catch (RuntimeException e) {
            close();
            throw e;
        }
    }

    /** Serial port of a DK (/dev/ttyACM0, COM3) at the default baud rate */
    public static LeaBench openSerial(String port) {
        return openSerial(port, DEFAULT_BAUD, null);
    }

    /**
     * Serial port of a DK.
     *
     * @param port /dev/ttyACM0, COM3, ...
     * @param baud baud rate (DEFAULT_BAUD for the DK)
     * @param log line log, may be null
     */
    public static LeaBench openSerial(String port, int baud, Consumer<String> log) {
        try {
            return new LeaBench(new SerialTransport(port, baud), log, SYNC_TIMEOUT);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    /** A simulated board through scripts/sim.py --tcp */
    public static LeaBench tcp(String host, int port) {
        return tcp(host, port, null);
    }

    /**
     * A simulated board through scripts/sim.py --tcp.
     *
     * @param host host of the bridge
     * @param port TCP port of the board (7000: leabench)
     * @param log line log, may be null
     */
    public static LeaBench tcp(String host, int port, Consumer<String> log) {
        try {
            return new LeaBench(new TcpTransport(host, port), log, SYNC_TIMEOUT);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    /** First DK (SEGGER USB VCOM) answering as leabench */
    public static LeaBench find() {
        return find(null);
    }

    /**
     * First DK answering as leabench with the given chip id.
     *
     * @param chipId value of the 'id' command, or null for any board
     */
    public static LeaBench find(String chipId) {
        List<String> errors = new ArrayList<>();
        for (SerialPort p : SerialPort.getCommPorts()) {
            if (SerialTransport.vendorId(p) != SEGGER_VID) {
                continue;
            }
            LeaBench tool;
            try {
                tool = new LeaBench(new SerialTransport(p.getSystemPortPath(), DEFAULT_BAUD), null, FIND_TIMEOUT);
            } catch (IOException | RuntimeException e) {
                errors.add(p.getSystemPortPath() + ": " + e.getMessage());
                continue; // another VCOM of the J-Link, or another firmware
            }
            if (chipId == null || chipId.equals(tool.id().getString("id"))) {
                return tool;
            }
            tool.close();
        }
        throw new IllegalStateException("no leabench board found" + (errors.isEmpty() ? "" : ": " + errors));
    }

    /** Reader thread: events go to the event log, everything else is a reply line */
    private void onLine(Object line) {
        if (line instanceof Map && "evt".equals(((Map<?, ?>) line).get("type"))) {
            events.add(toEvent((Map<?, ?>) line));
            return;
        }
        synchronized (replies) {
            replies.add(line);
            replies.notifyAll();
        }
    }

    private static Event toEvent(Map<?, ?> m) {
        Map<String, String> values = fields(m, "type", "t", "evt");
        Object t = m.get("t");
        return new Event(String.valueOf(m.get("evt")), t instanceof Number ? ((Number) t).longValue() : 0, values);
    }

    /** The values of a JSON line as strings, without the protocol keys */
    private static Map<String, String> fields(Map<?, ?> m, String... skip) {
        Map<String, String> out = new LinkedHashMap<>();
        for (Map.Entry<?, ?> e : m.entrySet()) {
            String k = String.valueOf(e.getKey());
            if (!List.of(skip).contains(k)) {
                out.put(k, String.valueOf(e.getValue()));
            }
        }
        return out;
    }

    // --- Life cycle ---

    @Override
    public void close() {
        reader.close();
        try {
            transport.close();
        } catch (IOException ignored) {
            // closing anyway
        }
    }

    /**
     * Waits until the board answers, switches it to JSON Lines and checks the protocol version.
     *
     * @param timeout how long to wait for the first answer
     * @return the reply of 'version'
     */
    public Reply sync(Duration timeout) {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (true) {
            try {
                raw("proto json", Duration.ofSeconds(1));
                break;
            } catch (LeaBenchException | IllegalStateException e) {
                if (System.nanoTime() > deadline) {
                    throw new IllegalStateException("board not responding", e);
                }
                send("");
            }
        }
        Reply v = raw("version", REPLY_TIMEOUT);
        if (v.getInt("proto") != Protocol.VERSION) {
            Map<String, String> f = new HashMap<>();
            f.put("board", v.getString("proto"));
            f.put("client", String.valueOf(Protocol.VERSION));
            throw new LeaBenchException("version", EINVAL, "protocol mismatch", f);
        }
        return v;
    }

    // --- Commands ---

    private void send(String line) {
        if (log != null) {
            log.accept(">> " + line);
        }
        try {
            transport.write((line + "\n").getBytes(StandardCharsets.US_ASCII));
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
    }

    @Override
    public Reply call(String command, List<Object> args, Map<String, Object> keys) {
        return raw(Line.build(command, args, keys), REPLY_TIMEOUT);
    }

    /** Sends any command line and waits for its final reply (ERR throws LeaBenchException). */
    public Reply raw(String line) {
        return raw(line, REPLY_TIMEOUT);
    }

    /**
     * Sends any command line and waits for its final reply.
     *
     * @param line command line, e.g. "bcast show 2"
     * @param timeout how long to wait for OK / ERR
     * @return the OK reply with the data lines before it
     * @throws LeaBenchException on ERR
     */
    public synchronized Reply raw(String line, Duration timeout) {
        String command = Line.commandOf(line, Protocol.WIRES);
        long deadline = System.nanoTime() + timeout.toNanos();
        List<Map<String, String>> data = new ArrayList<>();
        int idx;
        synchronized (replies) {
            idx = replies.size();
        }
        mark = events.size();
        send(line);
        synchronized (replies) {
            while (true) {
                while (idx < replies.size()) {
                    Object obj = replies.get(idx++);
                    Reply r;
                    try {
                        r = toReply(line, command, obj, data);
                    } catch (LeaBenchException e) {
                        replies.subList(0, idx).clear();
                        throw e;
                    }
                    if (r != null) {
                        // only lines of the next command are needed: long sessions stay small
                        replies.subList(0, idx).clear();
                        return r;
                    }
                }
                long remaining = deadline - System.nanoTime();
                if (remaining <= 0) {
                    throw new IllegalStateException("no reply to '" + line + "'");
                }
                try {
                    replies.wait(Math.max(1, remaining / 1_000_000));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("interrupted", e);
                }
            }
        }
    }

    /** The final reply if obj is one (data lines are collected), null to keep waiting */
    private static Reply toReply(String line, String command, Object obj, List<Map<String, String>> data) {
        if (obj instanceof String) {
            for (String e : SHELL_ERRORS) {
                if (((String) obj).contains(e)) {
                    throw new LeaBenchException(line, EINVAL, (String) obj, Collections.emptyMap());
                }
            }
            return null; // echo or other text
        }
        Map<?, ?> m = (Map<?, ?>) obj;
        Map<String, String> fields = fields(m, "type", "errno", "msg");
        Object type = m.get("type");
        if ("data".equals(type)) {
            data.add(fields);
            return null;
        }
        if ("err".equals(type)) {
            Object errno = m.get("errno");
            String msg = m.containsKey("msg") ? String.valueOf(m.get("msg")) : "";
            int code = errno instanceof Number ? ((Number) errno).intValue() : EINVAL;
            throw new LeaBenchException(line, code, msg, fields);
        }
        return "ok".equals(type) ? new Reply(command, fields, data) : null;
    }

    /** Reply lines kept for the next command (tests: must stay small) */
    int bufferedLines() {
        synchronized (replies) {
            return replies.size();
        }
    }

    // --- Events ---

    /** Position in the event list, for waitEvent(..., since, ...) */
    public int mark() {
        return events.size();
    }

    /** All events received so far */
    public List<Event> events() {
        return events.snapshot();
    }

    /**
     * Waits for an event received after the last command was sent.
     *
     * @param name event name (io.leabench.gen.Protocol.Events)
     * @param timeout how long to wait
     * @param keyValues fields that must be equal: key, value, key, value ... (booleans as 1/0)
     */
    public Event waitEvent(String name, Duration timeout, Object... keyValues) {
        return waitEvent(name, timeout, -1, keyValues);
    }

    /**
     * Waits for an event, counting from a mark() position.
     *
     * @param since position from mark(); negative: the last command sent
     */
    public Event waitEvent(String name, Duration timeout, int since, Object... keyValues) {
        Map<String, String> want = new LinkedHashMap<>();
        for (int i = 0; i + 1 < keyValues.length; i += 2) {
            Object v = keyValues[i + 1];
            String text = v instanceof Boolean ? ((Boolean) v ? "1" : "0") : String.valueOf(v);
            want.put(String.valueOf(keyValues[i]), text);
        }
        return waitEvent(name, timeout, since, e -> e.values().entrySet().containsAll(want.entrySet()));
    }

    /**
     * Waits for an event matching a predicate.
     *
     * @param since position from mark(); negative: the last command sent
     */
    public Event waitEvent(String name, Duration timeout, int since, Predicate<Event> match) {
        return events.await(name, timeout, since < 0 ? mark : since, match);
    }

    /** Queue receiving every event from now on (unsubscribe when done) */
    public BlockingQueue<Event> subscribe() {
        return events.subscribe();
    }

    /** Stops a queue from subscribe() */
    public void unsubscribe(BlockingQueue<Event> q) {
        events.unsubscribe(q);
    }

    /** Called from the reader thread for every event (keep it short) */
    public void addListener(Consumer<Event> listener) {
        events.addListener(listener);
    }

    /** Removes a listener */
    public void removeListener(Consumer<Event> listener) {
        events.removeListener(listener);
    }
}
