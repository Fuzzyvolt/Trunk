// Testbench for pltimer.v: acts as the AXI master (the CPU's role) and
// watches the irq output like the GIC would.
//
// Run on the laptop (needs Icarus Verilog: sudo apt install iverilog):
//   iverilog -o /tmp/tb tb_pltimer.v pltimer.v && vvp /tmp/tb
// Or in Vivado: add both files as simulation sources and Run Simulation.
//
// Prints PASS/FAIL per check and "ALL TESTS PASSED" at the end.

`timescale 1ns / 1ps

module tb_pltimer;
    reg clk = 0, rstn = 0;
    always #5 clk = ~clk;              // 100 MHz

    reg  [11:0] awaddr = 0, araddr = 0;
    reg  [31:0] wdata = 0;
    reg  [3:0]  wstrb = 0;
    reg         awvalid = 0, wvalid = 0, bready = 0, arvalid = 0, rready = 0;
    wire        awready, wready, bvalid, arready, rvalid;
    wire [1:0]  bresp, rresp;
    wire [31:0] rdata;
    wire        irq;

    pltimer dut (
        .s_axi_aclk(clk), .s_axi_aresetn(rstn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b000), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'b000), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .irq(irq)
    );

    localparam CTRL = 12'h000, PERIOD = 12'h004, STATUS = 12'h008,
               TICKS = 12'h00C, COUNT = 12'h010, ID = 12'h014;

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
            if (bresp !== 2'b00) begin $display("FAIL bresp=%b", bresp); errors = errors + 1; end
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

    task check(input [11:0] a, input [31:0] expect, input [8*32-1:0] what);
        reg [31:0] v;
        begin
            axi_read(a, v);
            if (v === expect) $display("PASS %0s: 0x%08x", what, v);
            else begin $display("FAIL %0s: got 0x%08x, expected 0x%08x", what, v, expect); errors = errors + 1; end
        end
    endtask

    task expect_bit(input b, input expect, input [8*32-1:0] what);
        begin
            if (b === expect) $display("PASS %0s: %0d", what, b);
            else begin $display("FAIL %0s: got %0d, expected %0d", what, b, expect); errors = errors + 1; end
        end
    endtask

    // Count rising edges of irq, and remember the clock number of each.
    integer cycle = 0, irq_edges = 0, last_edge = 0, gap = 0;
    reg irq_d = 0;
    always @(posedge clk) begin
        cycle <= cycle + 1;
        irq_d <= irq;
        if (irq && !irq_d) begin
            irq_edges <= irq_edges + 1;
            gap <= cycle - last_edge;
            last_edge <= cycle;
        end
    end

    reg [31:0] v, t1, t2;
    integer k;

    initial begin
        repeat (5) @(posedge clk);
        rstn <= 1;

        check(ID,     32'h4D5A0301, "ID");
        check(CTRL,   32'h0,        "CTRL after reset");
        check(PERIOD, 32'd100000000, "PERIOD after reset (1 s)");
        check(STATUS, 32'h0,        "STATUS after reset");
        expect_bit(irq, 0, "irq low after reset");

        // Timer stopped: COUNT stays 0.
        repeat (20) @(posedge clk);
        check(COUNT, 32'h0, "COUNT stays 0 while EN=0");

        // Short period, timer on but interrupt output off.
        axi_write(PERIOD, 32'd20);
        axi_write(TICKS, 32'd0);
        axi_write(CTRL, 32'b01);            // EN only
        repeat (30) @(posedge clk);
        check(STATUS, 32'h1, "PENDING set by a tick");
        expect_bit(irq, 0, "irq stays low with IE=0");

        // Interrupt on: irq goes high because PENDING is still set.
        axi_write(CTRL, 32'b11);
        repeat (2) @(posedge clk);
        expect_bit(irq, 1, "irq high with IE=1");

        // Level interrupt: still high many clocks later (nobody cleared it).
        repeat (50) @(posedge clk);
        expect_bit(irq, 1, "irq stays high until cleared");

        // Writing 0 to STATUS does nothing; writing 1 clears it.
        axi_write(STATUS, 32'h0);
        repeat (2) @(posedge clk);
        expect_bit(irq, 1, "write 0 to STATUS: no clear");

        // Behave like the driver: wait for irq, clear, repeat. Measure the spacing.
        axi_write(CTRL, 32'b00);            // stop (also resets COUNT)
        axi_write(STATUS, 32'h1);
        axi_write(TICKS, 32'd0);
        repeat (3) @(posedge clk);
        expect_bit(irq, 0, "irq low after clear + stop");
        axi_write(PERIOD, 32'd100);
        axi_write(CTRL, 32'b11);
        for (k = 0; k < 5; k = k + 1) begin
            while (!irq) @(posedge clk);
            axi_write(STATUS, 32'h1);
            repeat (3) @(posedge clk);
            if (irq) begin $display("FAIL irq still high after W1C"); errors = errors + 1; end
        end
        if (gap == 100) $display("PASS irq every PERIOD clocks: %0d", gap);
        else begin $display("FAIL irq spacing %0d, expected 100", gap); errors = errors + 1; end
        if (irq_edges >= 5) $display("PASS irq edges seen: %0d", irq_edges);
        else begin $display("FAIL only %0d irq edges", irq_edges); errors = errors + 1; end

        // TICKS counts every period, even ones the CPU did not clear in time
        // (this is how the driver spots missed interrupts).
        axi_read(TICKS, t1);
        repeat (1000) @(posedge clk);
        axi_read(TICKS, t2);
        if (t2 - t1 >= 10 && t2 - t1 <= 11) $display("PASS TICKS +%0d in ~1000 clocks", t2 - t1);
        else begin $display("FAIL TICKS %0d -> %0d", t1, t2); errors = errors + 1; end

        // Shrinking PERIOD below COUNT must not wait for a 2^32 wrap.
        axi_write(STATUS, 32'h1);
        axi_write(PERIOD, 32'd5000);
        repeat (300) @(posedge clk);         // COUNT now ~300
        axi_write(STATUS, 32'h1);
        axi_write(PERIOD, 32'd50);
        repeat (60) @(posedge clk);
        check(STATUS, 32'h1, "tick soon after PERIOD shrinks");

        // PERIOD 0 = no ticks.
        axi_write(PERIOD, 32'd0);
        axi_write(STATUS, 32'h1);
        axi_read(TICKS, t1);
        repeat (200) @(posedge clk);
        axi_read(TICKS, t2);
        if (t2 == t1) $display("PASS PERIOD=0: no ticks");
        else begin $display("FAIL PERIOD=0 ticked %0d -> %0d", t1, t2); errors = errors + 1; end

        check(12'h100, 32'h0, "unused offset");

        if (errors == 0) $display("ALL TESTS PASSED");
        else             $display("%0d TEST(S) FAILED", errors);
        $finish;
    end

    initial begin
        #200000 $display("FAIL: timeout (handshake stuck?)");
        $finish;
    end
endmodule
