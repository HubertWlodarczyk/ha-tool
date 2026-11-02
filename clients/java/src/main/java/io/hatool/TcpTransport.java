/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.hatool;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.util.Arrays;

/** A simulated board through scripts/sim.py --tcp. */
public final class TcpTransport implements Transport {
    private final Socket socket;
    private final InputStream in;
    private final OutputStream out;
    private final byte[] buf = new byte[4096];

    /** Connects to the bridge of a simulated board */
    public TcpTransport(String host, int port) throws IOException {
        socket = new Socket(host, port);
        socket.setTcpNoDelay(true);
        in = socket.getInputStream();
        out = socket.getOutputStream();
    }

    @Override
    public byte[] read(int timeoutMs) throws IOException {
        socket.setSoTimeout(timeoutMs);
        try {
            int n = in.read(buf);
            if (n < 0) {
                throw new IOException("connection closed");
            }
            return Arrays.copyOf(buf, n);
        } catch (SocketTimeoutException e) {
            return new byte[0];
        }
    }

    @Override
    public void write(byte[] data) throws IOException {
        out.write(data);
        out.flush();
    }

    @Override
    public void close() throws IOException {
        socket.close();
    }
}
