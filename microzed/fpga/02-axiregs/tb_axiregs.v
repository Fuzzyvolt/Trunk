// Testbench for axiregs.v: acts as the AXI master (the CPU's role).
//
// Run on the laptop (needs Icarus Verilog: sudo apt install iverilog):
//   iverilog -o /tmp/tb tb_axiregs.v axiregs.v && vvp /tmp/tb
// Or in Vivado: add both files as simulation sources and Run Simulation.
//
// Prints PASS/FAIL per check and "ALL TESTS PASSED" at the end.

`timescale 1ns / 1ps

module tb_axiregs;
    reg clk = 0, rstn = 0;
    always #5 clk = ~clk;              // 100 MHz

    reg  [11:0] awaddr = 0, araddr = 0;
    reg  [31:0] wdata = 0;
    reg  [3:0]  wstrb = 0;
    reg         awvalid = 0, wvalid = 0, bready = 0, arvalid = 0, rready = 0;
    wire        awready, wready, bvalid, arready, rvalid;
    wire [1:0]  bresp, rresp;
    wire [31:0] rdata;

    axiregs dut (
        .s_axi_aclk(clk), .s_axi_aresetn(rstn),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b000), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'b000), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready)
    );

    integer errors = 0;

    // AXI write. 'gap' delays WVALID after AWVALID to test that the slave waits for both.
    task axi_write(input [11:0] a, input [31:0] d, input [3:0] s, input integer gap);
        begin
            @(posedge clk);
            awaddr <= a; awvalid <= 1;
            repeat (gap) @(posedge clk);
            wdata <= d; wstrb <= s; wvalid <= 1;
            // Hold VALID until the handshake edge (VALID && READY).
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

    task check(input [11:0] a, input [31:0] expect, input [8*24-1:0] what);
        reg [31:0] v;
        begin
            axi_read(a, v);
            if (v === expect) $display("PASS %0s: 0x%08x", what, v);
            else begin $display("FAIL %0s: got 0x%08x, expected 0x%08x", what, v, expect); errors = errors + 1; end
        end
    endtask

    reg [31:0] c1, c2;

    initial begin
        repeat (5) @(posedge clk);
        rstn <= 1;

        check(12'h010, 32'h4D5A0201, "ID");
        check(12'h000, 32'h0,        "REG0 after reset");

        axi_write(12'h000, 32'h12345678, 4'b1111, 0);
        axi_write(12'h004, 32'h00000010, 4'b1111, 3);   // W arrives 3 clocks after AW
        check(12'h000, 32'h12345678, "REG0");
        check(12'h004, 32'h00000010, "REG1");
        check(12'h008, 32'h12345688, "SUM");

        axi_write(12'h000, 32'hAABBCCDD, 4'b0010, 0);   // only byte 1
        check(12'h000, 32'h1234CC78, "REG0 byte strobe");

        axi_write(12'h008, 32'hFFFFFFFF, 4'b1111, 0);   // SUM is read-only
        check(12'h008, 32'h1234CC88, "SUM ignores writes");
        check(12'h100, 32'h0,        "unused offset");

        axi_read(12'h00C, c1);
        repeat (100) @(posedge clk);
        axi_read(12'h00C, c2);
        if (c2 - c1 >= 100 && c2 - c1 < 120) $display("PASS COUNT advances: +%0d", c2 - c1);
        else begin $display("FAIL COUNT: %0d then %0d", c1, c2); errors = errors + 1; end

        if (errors == 0) $display("ALL TESTS PASSED");
        else             $display("%0d TEST(S) FAILED", errors);
        $finish;
    end

    initial begin
        #100000 $display("FAIL: timeout (handshake stuck?)");
        $finish;
    end
endmodule
