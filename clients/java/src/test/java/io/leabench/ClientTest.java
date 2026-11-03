/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.leabench.gen.Protocol;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;
import org.junit.jupiter.api.Test;

/** Unit tests of the client on a scripted fake board (no hardware, no simulation). */
class ClientTest {
    private static final Duration SHORT = Duration.ofMillis(300);
    private static final Duration WAIT = Duration.ofSeconds(2);

    /** Answers each command line with the lines handler(line) returns (JSON or text). */
    static final class FakeBoard implements Transport {
        private final LinkedBlockingQueue<byte[]> rx = new LinkedBlockingQueue<>();
        final List<String> sent = new ArrayList<>();
        Function<String, List<String>> handler;
        int protocol = Protocol.VERSION;
        volatile boolean closed;
        int chunk;

        FakeBoard(Function<String, List<String>> handler) {
            this.handler = handler;
        }

        void emit(String... lines) {
            StringBuilder sb = new StringBuilder();
            for (String l : lines) {
                sb.append(l).append("\r\n");
            }
            byte[] data = sb.toString().getBytes(StandardCharsets.UTF_8);
            if (chunk == 0) {
                rx.add(data);
                return;
            }
            for (int i = 0; i < data.length; i += chunk) {
                byte[] part = new byte[Math.min(chunk, data.length - i)];
                System.arraycopy(data, i, part, 0, part.length);
                rx.add(part);
            }
        }

        @Override
        public byte[] read(int timeoutMs) throws IOException {
            if (closed) {
                throw new IOException("closed");
            }
            try {
                byte[] b = rx.poll(timeoutMs, TimeUnit.MILLISECONDS);
                return b == null ? new byte[0] : b;
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new IOException(e);
            }
        }

        @Override
        public synchronized void write(byte[] data) {
            for (String line : new String(data, StandardCharsets.US_ASCII).split("\n")) {
                sent.add(line);
                if (line.equals("proto json")) {
                    emit("{\"type\":\"ok\",\"proto\":\"json\"}");
                } else if (line.equals("version")) {
                    emit("{\"type\":\"ok\",\"app\":\"leabench\",\"version\":\"0.2.0\",\"zephyr\":\"4.5.0\","
                            + "\"proto\":\"" + protocol + "\"}");
                } else if (!line.isEmpty()) {
                    emit(handler.apply(line).toArray(new String[0]));
                }
            }
        }

        @Override
        public void close() {
            closed = true;
        }
    }

    private static LeaBench open(FakeBoard fake) {
        return new LeaBench(fake, null, WAIT);
    }

    @Test
    void syncSwitchesToJson() {
        FakeBoard fake = new FakeBoard(l -> List.of());
        LeaBench tool = open(fake);
        assertEquals(List.of("proto json", "version"), fake.sent.subList(0, 2));
        tool.close();
        assertTrue(fake.closed);
    }

    @Test
    void protocolMismatchRefused() {
        FakeBoard fake = new FakeBoard(l -> List.of());
        fake.protocol = Protocol.VERSION + 1;
        LeaBenchException e = assertThrows(LeaBenchException.class, () -> open(fake));
        assertTrue(e.message().contains("protocol mismatch"));
        assertTrue(fake.closed);
    }

    @Test
    void replyValuesAndDataRows() {
        FakeBoard fake = new FakeBoard(l -> List.of(
                "bonding list",
                "{\"type\":\"data\",\"tag\":\"IDENTITY\",\"id\":\"1\",\"name\":\"A b\",\"bonds\":\"2\"}",
                "{\"type\":\"ok\",\"count\":\"1\"}"));
        LeaBench tool = open(fake);
        Reply r = tool.raw("bonding list");
        assertEquals(1, r.getInt("count"));
        List<Values> rows = r.rows("IDENTITY");
        assertEquals(1, rows.size());
        assertEquals(2, rows.get(0).getInt("bonds"));
        assertEquals("A b", rows.get(0).getString("name"));
        tool.close();
    }

    @Test
    void errThrowsWithErrnoAndFields() {
        FakeBoard fake = new FakeBoard(l -> List.of("{\"type\":\"err\",\"errno\":-22,\"msg\":\"bad\",\"key\":\"x\"}"));
        LeaBench tool = open(fake);
        LeaBenchException e = assertThrows(LeaBenchException.class, () -> tool.raw("broadcast 1 create bogus=1"));
        assertEquals(-22, e.errno());
        assertEquals("bad", e.message());
        assertEquals(Map.of("key", "x"), e.fields());
        tool.close();
    }

    @Test
    void everyShellErrorEndsTheCommand() {
        for (String text : List.of("x: command not found", "list: wrong parameter count",
                "bcast: Please specify a subcommand.", "line ignored: 1 non-ASCII character(s)",
                "not terminated: \"", "Too many arguments in the command.")) {
            FakeBoard fake = new FakeBoard(l -> List.of(text));
            LeaBench tool = open(fake);
            LeaBenchException e = assertThrows(LeaBenchException.class, () -> tool.raw("whatever", WAIT), text);
            assertEquals(-22, e.errno());
            tool.close();
        }
    }

    @Test
    void noReplyTimesOut() {
        LeaBench tool = open(new FakeBoard(l -> List.of()));
        assertThrows(IllegalStateException.class, () -> tool.raw("status", SHORT));
        tool.close();
    }

    @Test
    void eventsDuringCommandAndWaitEvent() {
        FakeBoard fake = new FakeBoard(l -> List.of(
                "{\"type\":\"evt\",\"t\":5,\"evt\":\"broadcast_started\",\"big\":\"1\",\"encrypted\":\"0\"}",
                "{\"type\":\"ok\",\"queued\":\"1\"}"));
        LeaBench tool = open(fake);
        tool.raw("broadcast 1 start");
        Event e = tool.waitEvent("broadcast_started", WAIT, "big", 1, "encrypted", false);
        assertEquals(5, e.timeMs());
        assertFalse(e.getBool("encrypted"));
        assertThrows(IllegalStateException.class, () -> tool.waitEvent("broadcast_started", SHORT, "big", 2));
        int m = tool.mark();
        fake.emit("{\"type\":\"evt\",\"t\":9,\"evt\":\"broadcast_stopped\",\"big\":\"1\"}");
        assertEquals(9, tool.waitEvent("broadcast_stopped", WAIT, m, ev -> ev.getInt("big") == 1).timeMs());
        tool.close();
    }

    @Test
    void subscribeAndFailingListener() throws InterruptedException {
        FakeBoard fake = new FakeBoard(l -> List.of());
        LeaBench tool = open(fake);
        List<String> seen = new ArrayList<>();
        tool.addListener(ev -> {
            throw new IllegalStateException("listener bug");
        });
        tool.addListener(ev -> {
            synchronized (seen) {
                seen.add(ev.name());
            }
        });
        BlockingQueue<Event> q = tool.subscribe();
        fake.emit("{\"type\":\"evt\",\"t\":1,\"evt\":\"mode\",\"to\":\"unicast\"}",
                "{\"type\":\"evt\",\"t\":2,\"evt\":\"mode\",\"to\":\"none\"}");
        assertEquals("unicast", q.poll(2, TimeUnit.SECONDS).getString("to"));
        assertEquals("none", q.poll(2, TimeUnit.SECONDS).getString("to"));
        tool.unsubscribe(q);
        Thread.sleep(100);
        synchronized (seen) {
            assertEquals(List.of("mode", "mode"), seen); // the failing listener did not stop the reader
        }
        tool.close();
    }

    @Test
    void linesSplitAcrossReads() {
        FakeBoard fake = new FakeBoard(l -> List.of(
                "{\"type\":\"data\",\"tag\":\"BIG\",\"name\":\"Ż ó\"}", "{\"type\":\"ok\",\"count\":\"1\"}"));
        fake.chunk = 3;
        LeaBench tool = open(fake);
        assertEquals("Ż ó", tool.raw("broadcast list").data().get(0).get("name"));
        tool.close();
    }

    @Test
    void commandsSerialisedAcrossThreads() throws InterruptedException {
        FakeBoard fake = new FakeBoard(l -> List.of("{\"type\":\"ok\",\"echo\":\"" + l.split(" ")[1] + "\"}"));
        LeaBench tool = open(fake);
        Map<Integer, String> results = new ConcurrentHashMap<>();
        List<Thread> threads = new ArrayList<>();
        for (int i = 0; i < 20; i++) {
            final int n = i;
            Thread t = new Thread(() -> results.put(n, tool.raw("time " + n).getString("echo")));
            threads.add(t);
            t.start();
        }
        for (Thread t : threads) {
            t.join();
        }
        for (int i = 0; i < 20; i++) {
            assertEquals(String.valueOf(i), results.get(i));
        }
        tool.close();
    }

    @Test
    void replyBufferDoesNotGrow() {
        FakeBoard fake = new FakeBoard(l -> List.of("echo " + l, "{\"type\":\"ok\"}"));
        LeaBench tool = open(fake);
        for (int i = 0; i < 500; i++) {
            tool.raw("time " + i);
        }
        assertTrue(tool.bufferedLines() < 5, String.valueOf(tool.bufferedLines()));
        tool.close();
    }
}
