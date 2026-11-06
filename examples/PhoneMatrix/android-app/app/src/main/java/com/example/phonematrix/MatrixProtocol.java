package com.example.phonematrix;

import java.util.UUID;

/** The binary protocol shared with the board's PhoneMatrix BLE sketch. */
public final class MatrixProtocol {
    public static final int ROWS = 8, COLUMNS = 12, RAW_LENGTH = 13;
    public static final UUID SERVICE = UUID.fromString("19B10000-E8F2-537E-4F6C-D104768A1214");
    public static final UUID COMMAND = UUID.fromString("19B10001-E8F2-537E-4F6C-D104768A1214");
    public static final UUID STATUS = UUID.fromString("19B10002-E8F2-537E-4F6C-D104768A1214");
    public static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB");
    private MatrixProtocol() {}

    public static byte[] preset(int opcode) {
        if (opcode < 0 || opcode > 3) throw new IllegalArgumentException("opcode must be 0..3");
        return new byte[]{(byte) opcode};
    }

    public static byte[] raw(boolean[][] pixels) {
        if (pixels == null || pixels.length != ROWS) throw new IllegalArgumentException("8 rows required");
        byte[] out = new byte[RAW_LENGTH]; out[0] = 0x10;
        for (int r = 0; r < ROWS; r++) {
            if (pixels[r] == null || pixels[r].length != COLUMNS) throw new IllegalArgumentException("12 columns required");
            for (int c = 0; c < COLUMNS; c++) if (pixels[r][c]) {
                int i = r * COLUMNS + c; out[1 + i / 8] |= (byte) (1 << (7 - i % 8));
            }
        }
        return out;
    }

    public static String expectedStatus(byte[] value) {
        if (value == null) throw new IllegalArgumentException("null command");
        if (value.length == 1) switch (value[0] & 0xff) {
            case 0: return "OK:CLEAR"; case 1: return "OK:HEART";
            case 2: return "OK:SMILE"; case 3: return "OK:CHECKER";
        }
        if (value.length == RAW_LENGTH && value[0] == 0x10) return "OK:RAW";
        throw new IllegalArgumentException("invalid command length/opcode");
    }

    public static String hex(byte[] value) {
        StringBuilder b = new StringBuilder();
        for (byte v : value) { if (b.length() > 0) b.append(' '); b.append(String.format("%02X", v & 0xff)); }
        return b.toString();
    }
}
