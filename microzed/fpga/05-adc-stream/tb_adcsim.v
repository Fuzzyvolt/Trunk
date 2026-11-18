// Testbench for adcsim.v: plays the CPU (AXI4-Lite master) and the DMA
// (AXI4-Stream receiver, with a TREADY it can pull low to stall the stream).
//
// Run on the laptop (needs Icarus Verilog: sudo apt install iverilog):
//   iverilog -o /tmp/tb tb_adcsim.v adcsim.v && vvp /tmp/tb
//
// Prints PASS/FAIL per check and "ALL TESTS PASSED" at the end.

`timescale 1ns / 1ps

module tb_adcsim;
    reg clk = 0, rstn = 0;
    always #5 clk = ~clk;              // 100 MHz

    reg  [11:0] awaddr = 0, araddr = 0;
    reg  [31:0] wdata = 0;
    reg  [3:0]  wstrb = 0;
    reg         awvalid = 0, wvalid = 0, bready = 0, arvalid = 0, rready = 0;
    wire        awready, wready, bvalid, arready, rvalid;
    wire [1:0]  bresp, rresp;
    wire [31:0] rdata;

    wire [31:0] tdata;
    wire [3:0]  tkeep;
    wire        tlast, tvalid;
    reg         tready = 1;
    wire        fifo_aresetn;

    adcsim dut (
        .s_axi_aclk(clk), .s_axi_aresetn(rstn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b000), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'b000), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .m_axis_tdata(tdata), .m_axis_tkeep(tkeep), .m_axis_tlast(tlast),
        .m_axis_tvalid(tvalid), .m_axis_tready(tready),
        .fifo_aresetn(fifo_aresetn)
    );

    localparam CTRL = 12'h000, STATUS = 12'h004, DIV = 12'h008, PKT_LEN = 12'h00C,
               PHASE_INC = 12'h010, SAMPLES = 12'h014, DROPPED = 12'h018,
               PACKETS = 12'h01C, ID = 12'h020;
    localparam EN = 1, SINE = 2, FLUSH = 4;

    integer errors = 0;

    task axi_write(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk);
            awaddr <= a; awvalid <= 1;
            wdata <= d; wstrb <= 4'b1111; wvalid <= 1;
            @(posedge clk);
            while (!(awready && wready)) @(posedge clk);
            awvalid <= 0; wvalid <= 0;
            bready <= 1;
            @(posedge clk);
            while (!bvalid) @(posedge clk);
            bready <= 0;
        end
    endtask

    task axi_read(input [11:0] a, output [31:0] d);
        begin
            @(posedge clk);
            araddr <= a; arvalid <= 1;
            @(posedge clk);
            while (!arready) @(posedge clk);
            arvalid <= 0;
            rready <= 1;
            @(posedge clk);
            while (!rvalid) @(posedge clk);
            d = rdata;
            rready <= 0;
        end
    endtask

    task check(input [11:0] a, input [31:0] expect, input [8*64-1:0] what);
        reg [31:0] v;
        begin
            axi_read(a, v);
            if (v === expect) $display("PASS %0s: %0d (0x%08x)", what, v, v);
            else begin $display("FAIL %0s: got %0d, expected %0d", what, v, expect); errors = errors + 1; end
        end
    endtask

    task expect_int(input integer got, input integer expect, input [8*64-1:0] what);
        begin
            if (got === expect) $display("PASS %0s: %0d", what, got);
            else begin $display("FAIL %0s: got %0d, expected %0d", what, got, expect); errors = errors + 1; end
        end
    endtask

    // ---------------- Stream monitor (the "DMA") ----------------
    // Checks every word that is handed over: sequence numbers, packet length,
    // TKEEP, and the value expected for that sequence number.
    integer words = 0, pkts = 0, inpkt = 0, gaps = 0, badlen = 0, badval = 0;
    integer cycle = 0, last_cycle = 0, spacing = 0;
    integer pkt_len_exp = 8;
    reg     first = 1;
    reg [15:0] next_seq = 0;
    reg [31:0] inc_tb = 0;
    reg        sine_tb = 0;
    reg [31:0] ph;
    reg signed [15:0] expv;
    real       r;

    always @(posedge clk) cycle <= cycle + 1;

    always @(posedge clk) begin
        if (tvalid && tready) begin
            words = words + 1;
            spacing = cycle - last_cycle;
            last_cycle = cycle;
            if (tkeep !== 4'hF) badval = badval + 1;
            // Sequence: count samples lost between two received words.
            if (!first && tdata[31:16] != next_seq)
                gaps = gaps + (tdata[31:16] - next_seq);
            first = 0;
            next_seq = tdata[31:16] + 16'd1;
            // Expected value for this sequence number (phase = seq * PHASE_INC).
            ph = tdata[31:16] * inc_tb;
            if (sine_tb) begin
                r = 32767.0 * $sin(2.0 * 3.14159265358979 * ph[31:24] / 256.0);
                expv = r;               // real -> integer rounds to nearest
            end else begin
                expv = {~ph[31], ph[30:16]};
            end
            if (tdata[15:0] !== expv) begin
                if (badval < 5) $display("  bad value seq %0d: got %0d expected %0d",
                                         tdata[31:16], $signed(tdata[15:0]), expv);
                badval = badval + 1;
            end
            inpkt = inpkt + 1;
            if (tlast) begin
                if (inpkt != pkt_len_exp) badlen = badlen + 1;
                pkts = pkts + 1;
                inpkt = 0;
            end
        end
    end

    task reset_monitor;
        begin
            words = 0; pkts = 0; inpkt = 0; gaps = 0; badlen = 0; badval = 0; first = 1;
        end
    endtask

    reg [31:0] v, v2;
    integer k;

    initial begin
        repeat (5) @(posedge clk);
        rstn <= 1;
        repeat (2) @(posedge clk);

        $display("--- registers after reset");
        check(ID,        32'h4D5A0501, "ID");
        check(CTRL,      0,            "CTRL");
        check(STATUS,    0,            "STATUS (not running)");
        check(DIV,       100,          "DIV (1 MSa/s)");
        check(PKT_LEN,   4096,         "PKT_LEN");
        check(PHASE_INC, 4294967,      "PHASE_INC (1 kHz)");
        expect_int(tvalid, 0, "no stream while stopped");
        expect_int(fifo_aresetn, 1, "FIFO not in reset");

        $display("--- sawtooth, a sample every 4 clocks, 8 per packet, DMA always ready");
        inc_tb = 32'h1000_0000;           // 16 samples per period
        sine_tb = 0;
        pkt_len_exp = 8;
        axi_write(DIV, 4);
        axi_write(PKT_LEN, 8);
        axi_write(PHASE_INC, inc_tb);
        reset_monitor;
        axi_write(CTRL, EN);
        repeat (400) @(posedge clk);
        check(STATUS, 1, "RUNNING");
        expect_int(spacing, 4, "clocks between samples");
        // Stop mid-packet: the packet must still be completed.
        axi_write(CTRL, 0);
        repeat (100) @(posedge clk);
        check(STATUS, 0, "stopped after the packet ended");
        expect_int(words % 8, 0, "whole packets only");
        expect_int(inpkt, 0, "no half packet left");
        expect_int(badlen, 0, "packets with wrong length");
        expect_int(badval, 0, "wrong values");
        expect_int(gaps, 0, "sequence gaps");
        axi_read(SAMPLES, v);
        expect_int(v, words, "SAMPLES = words received");
        axi_read(PACKETS, v);
        expect_int(v, pkts, "PACKETS = packets received");
        check(DROPPED, 0, "DROPPED");
        $display("  %0d words, %0d packets", words, pkts);

        $display("--- DMA stalls for 200 clocks: samples dropped and counted");
        reset_monitor;
        axi_write(CTRL, EN);
        repeat (100) @(posedge clk);
        tready <= 0;
        repeat (200) @(posedge clk);
        tready <= 1;
        repeat (100) @(posedge clk);
        axi_write(CTRL, 0);
        repeat (100) @(posedge clk);
        axi_read(DROPPED, v);
        if (v > 40 && v < 60) $display("PASS DROPPED about 200/4: %0d", v);
        else begin $display("FAIL DROPPED: %0d (expected about 50)", v); errors = errors + 1; end
        expect_int(gaps, v, "sequence gap = DROPPED");
        axi_read(SAMPLES, v2);
        expect_int(v2 - v, words, "SAMPLES - DROPPED = words received");
        expect_int(badlen, 0, "packets still 8 words");
        expect_int(badval, 0, "wrong values");
        check(STATUS, 0, "stopped");

        $display("--- stop while the DMA is stalled: only FLUSH ends it");
        reset_monitor;
        axi_write(CTRL, EN);
        repeat (50) @(posedge clk);
        tready <= 0;
        repeat (10) @(posedge clk);
        axi_write(CTRL, 0);
        repeat (100) @(posedge clk);
        check(STATUS, 1, "still RUNNING (packet cannot finish)");
        axi_write(CTRL, FLUSH);
        @(posedge clk); @(posedge clk);
        expect_int(fifo_aresetn, 0, "FIFO reset during FLUSH");
        expect_int(tvalid, 0, "output emptied");
        check(STATUS, 0, "not running after FLUSH");
        axi_write(CTRL, 0);
        tready <= 1;
        repeat (3) @(posedge clk);
        expect_int(fifo_aresetn, 1, "FIFO out of reset");

        $display("--- restart: counters and sequence start again from 0");
        reset_monitor;
        axi_write(CTRL, EN);
        @(posedge tvalid);
        @(negedge clk);
        expect_int(tdata[31:16], 0, "first sequence number");
        axi_write(CTRL, 0);
        repeat (100) @(posedge clk);
        expect_int(gaps, 0, "sequence gaps");
        expect_int(badlen, 0, "packet lengths");

        $display("--- sine, one table step per sample, a sample every clock (DIV 1)");
        inc_tb = 32'h0100_0000;
        sine_tb = 1;
        pkt_len_exp = 64;
        axi_write(DIV, 1);
        axi_write(PKT_LEN, 64);
        axi_write(PHASE_INC, inc_tb);
        reset_monitor;
        axi_write(CTRL, EN | SINE);
        repeat (600) @(posedge clk);
        axi_write(CTRL, SINE);
        repeat (100) @(posedge clk);
        expect_int(spacing, 1, "clocks between samples");
        expect_int(badval, 0, "sine values match 32767*sin()");
        expect_int(badlen, 0, "packets of 64");
        expect_int(gaps, 0, "sequence gaps");
        check(DROPPED, 0, "DROPPED at 100 MSa/s with the DMA always ready");
        $display("  %0d words, %0d packets", words, pkts);

        $display("--- DIV 0 and PKT_LEN 0 count as 1 (no lock-up)");
        pkt_len_exp = 1;
        sine_tb = 0;
        axi_write(DIV, 0);
        axi_write(PKT_LEN, 0);
        reset_monitor;
        axi_write(CTRL, EN);
        repeat (50) @(posedge clk);
        axi_write(CTRL, 0);
        repeat (20) @(posedge clk);
        if (words > 30) $display("PASS samples flow: %0d", words);
        else begin $display("FAIL only %0d samples", words); errors = errors + 1; end
        expect_int(badlen, 0, "every word has TLAST");

        if (errors == 0) $display("ALL TESTS PASSED");
        else             $display("%0d TEST(S) FAILED", errors);
        $finish;
    end

    initial begin
        #2_000_000;
        $display("FAIL: timeout");
        $finish;
    end
endmodule
