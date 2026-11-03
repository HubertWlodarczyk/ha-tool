/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.function.Consumer;
import java.util.function.Predicate;

/** Every event received, in order, with waiting, subscription queues and listeners. */
final class EventLog {
    private final List<Event> events = new ArrayList<>();
    private final List<Consumer<Event>> listeners = new CopyOnWriteArrayList<>();
    private final List<BlockingQueue<Event>> queues = new CopyOnWriteArrayList<>();

    /** Called from the reader thread */
    void add(Event ev) {
        synchronized (events) {
            events.add(ev);
            events.notifyAll();
        }
        for (BlockingQueue<Event> q : queues) {
            q.offer(ev);
        }
        for (Consumer<Event> l : listeners) {
            try {
                l.accept(ev);
            } catch (RuntimeException ignored) {
                // a listener must not stop the reader
            }
        }
    }

    int size() {
        synchronized (events) {
            return events.size();
        }
    }

    List<Event> snapshot() {
        synchronized (events) {
            return new ArrayList<>(events);
        }
    }

    /** First event named name at or after position since that matches */
    Event await(String name, Duration timeout, int since, Predicate<Event> match) {
        long deadline = System.nanoTime() + timeout.toNanos();
        synchronized (events) {
            int idx = since;
            while (true) {
                while (idx < events.size()) {
                    Event e = events.get(idx++);
                    if (e.name().equals(name) && match.test(e)) {
                        return e;
                    }
                }
                long remaining = deadline - System.nanoTime();
                if (remaining <= 0) {
                    throw new IllegalStateException("no event " + name + " within " + timeout);
                }
                try {
                    events.wait(Math.max(1, remaining / 1_000_000));
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new IllegalStateException("interrupted", e);
                }
            }
        }
    }

    BlockingQueue<Event> subscribe() {
        BlockingQueue<Event> q = new LinkedBlockingQueue<>();
        queues.add(q);
        return q;
    }

    void unsubscribe(BlockingQueue<Event> q) {
        queues.remove(q);
    }

    void addListener(Consumer<Event> listener) {
        listeners.add(listener);
    }

    void removeListener(Consumer<Event> listener) {
        listeners.remove(listener);
    }
}
