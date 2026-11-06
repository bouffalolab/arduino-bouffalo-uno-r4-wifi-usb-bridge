package com.example.phonematrix;

/** Same row order and preset pixels as the RA4M1 PhoneMatrix firmware. */
public final class PresetPatterns {
    private static final String[][] ROWS = {
        {"000000000000","000000000000","000000000000","000000000000",
         "000000000000","000000000000","000000000000","000000000000"},
        {"000110011000","001111111100","001111111100","001111111100",
         "000111111000","000011110000","000001100000","000000000000"},
        {"000000000000","000110011000","000110011000","000000000000",
         "000000000000","001000000100","000100001000","000011110000"},
        {"101010101010","010101010101","101010101010","010101010101",
         "101010101010","010101010101","101010101010","010101010101"}
    };
    private PresetPatterns() {}
    public static boolean[][] pixels(int opcode) {
        if (opcode<0||opcode>3) throw new IllegalArgumentException("Preset must be 0..3");
        boolean[][] pixels=new boolean[MatrixProtocol.ROWS][MatrixProtocol.COLUMNS];
        for(int r=0;r<8;r++)for(int c=0;c<12;c++)pixels[r][c]=ROWS[opcode][r].charAt(c)=='1';
        return pixels;
    }
}
