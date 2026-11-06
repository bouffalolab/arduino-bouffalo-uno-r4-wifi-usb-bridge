package com.example.phonematrix;

/** 5x7 digits placed within the existing 8-row, 12-column RAW bitmap. */
public final class DigitPatterns {
    private static final String[][] GLYPHS = {
        {"01110","10001","10011","10101","11001","10001","01110"},
        {"00100","01100","00100","00100","00100","00100","01110"},
        {"01110","10001","00001","00010","00100","01000","11111"},
        {"11110","00001","00001","01110","00001","00001","11110"},
        {"00010","00110","01010","10010","11111","00010","00010"},
        {"11111","10000","10000","11110","00001","00001","11110"},
        {"01110","10000","10000","11110","10001","10001","01110"},
        {"11111","00001","00010","00100","01000","01000","01000"},
        {"01110","10001","10001","01110","10001","10001","01110"},
        {"01110","10001","10001","01111","00001","00001","01110"}
    };
    private DigitPatterns() {}
    public static boolean[][] pixels(int digit) {
        if (digit < 0 || digit > 9) throw new IllegalArgumentException("Digit must be 0..9");
        boolean[][] pixels = new boolean[MatrixProtocol.ROWS][MatrixProtocol.COLUMNS];
        for (int row=0;row<7;row++) for (int column=0;column<5;column++)
            pixels[row][column+3] = GLYPHS[digit][row].charAt(column)=='1';
        return pixels;
    }
}
