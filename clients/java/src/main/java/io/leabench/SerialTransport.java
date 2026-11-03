/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

package io.leabench;

import com.fazecast.jSerialComm.SerialPort;
import java.io.IOException;
import java.util.Arrays;

/** Serial port of a DK (jSerialComm: /dev/ttyACM0, COM3, ...), 1 Mbaud by default. */
public final class SerialTransport implements Transport {
    private final SerialPort port;
    private final byte[] buf = new byte[4096];

    /** Opens a serial port (8N1, no flow control) */
    public SerialTransport(String name, int baud) throws IOException {
        port = SerialPort.getCommPort(name);
        port.setComPortParameters(baud, 8, SerialPort.ONE_STOP_BIT, SerialPort.NO_PARITY);
        port.setFlowControl(SerialPort.FLOW_CONTROL_DISABLED);
        if (!port.openPort()) {
            throw new IOException("cannot open " + name);
        }
    }

    /** USB vendor id of the port (0x1366: SEGGER J-Link OB of the DK), -1 if unknown */
    static int vendorId(SerialPort p) {
        try {
            return p.getVendorID();
        } catch (RuntimeException e) {
            return -1;
        }
    }

    @Override
    public byte[] read(int timeoutMs) throws IOException {
        port.setComPortTimeouts(SerialPort.TIMEOUT_READ_SEMI_BLOCKING, timeoutMs, 0);
        int n = port.readBytes(buf, buf.length);
        if (n < 0) {
            throw new IOException("serial port closed");
        }
        return Arrays.copyOf(buf, n);
    }

    @Override
    public void write(byte[] data) throws IOException {
        if (port.writeBytes(data, data.length) != data.length) {
            throw new IOException("serial write failed");
        }
    }

    @Override
    public void close() {
        port.closePort();
    }
}
