/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import java.io.Closeable;
import java.io.IOException;

/** Byte stream to a board: a serial port (DK) or TCP (simulation bridge). */
public interface Transport extends Closeable {
    /** Bytes read within timeoutMs (empty if none); IOException when the link is gone */
    byte[] read(int timeoutMs) throws IOException;

    void write(byte[] data) throws IOException;
}
