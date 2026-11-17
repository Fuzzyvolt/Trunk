// axiregs: a small AXI4-Lite slave with five 32-bit registers (project 02).
//
// Offset  Name   Access  What it holds
// 0x00    REG0   R/W     scratch register
// 0x04    REG1   R/W     scratch register
// 0x08    SUM    R       REG0 + REG1, computed in logic
// 0x0C    COUNT  R       free-running counter, +1 every clock (100 MHz FCLK_CLK0)
// 0x10    ID     R       constant 0x4D5A0201 ("MZ", project 02, version 1)
// others         R       0 (writes are ignored)
//
// The CPU sees these at 0x43C00000 + offset (address set in build.tcl).
//
// AXI4-Lite in short: five channels, each with a VALID/READY handshake.
// A transfer happens on the clock edge where both VALID and READY are high.
//   write: AW (address) + W (data)  -> B (response)
//   read:  AR (address)             -> R (data + response)
// This slave handles one transfer at a time, which is plenty for a CPU poking registers.
//
// The s_axi_* port names let Vivado recognise the ports as one AXI interface
// ("s_axi") when this file is added to a block design as a module reference.

`timescale 1ns / 1ps

module axiregs #(
    parameter [31:0] ID = 32'h4D5A_0201
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s_axi_aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi, ASSOCIATED_RESET s_axi_aresetn" *)
    input  wire        s_axi_aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 s_axi_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        s_axi_aresetn,

    // Write address channel
    input  wire [11:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    // Write data channel
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    // Write response channel
    output wire [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    // Read address channel
    input  wire [11:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    // Read data channel
    output reg  [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready
);

    wire clk = s_axi_aclk;
    wire rst = ~s_axi_aresetn;

    assign s_axi_bresp = 2'b00;   // OKAY
    assign s_axi_rresp = 2'b00;   // OKAY

    reg [31:0] reg0, reg1, count;
    wire [31:0] sum = reg0 + reg1;

    // Free-running counter: proves the PL clock runs and the logic is alive.
    always @(posedge clk)
        if (rst) count <= 32'd0;
        else     count <= count + 32'd1;

    // ---------------- Write ----------------
    // Wait until both address and data are VALID, then raise AWREADY and WREADY
    // together for one clock. That clock is the handshake: store the data.
    // Then hold BVALID until the master takes the response (BREADY).
    wire wr_en = s_axi_awvalid && s_axi_awready && s_axi_wvalid && s_axi_wready;

    always @(posedge clk) begin
        if (rst) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
        end else begin
            if (!s_axi_awready && s_axi_awvalid && s_axi_wvalid && !s_axi_bvalid) begin
                s_axi_awready <= 1'b1;
                s_axi_wready  <= 1'b1;
            end else begin
                s_axi_awready <= 1'b0;
                s_axi_wready  <= 1'b0;
            end

            if (wr_en)
                s_axi_bvalid <= 1'b1;
            else if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;
        end
    end

    // WSTRB says which bytes of WDATA are valid (bit 0 = bits 7:0, ...).
    // A 32-bit CPU store sets all four.
    integer i;
    always @(posedge clk) begin
        if (rst) begin
            reg0 <= 32'd0;
            reg1 <= 32'd0;
        end else if (wr_en) begin
            for (i = 0; i < 4; i = i + 1)
                if (s_axi_wstrb[i])
                    case (s_axi_awaddr[11:2])
                        10'h0: reg0[i*8 +: 8] <= s_axi_wdata[i*8 +: 8];
                        10'h1: reg1[i*8 +: 8] <= s_axi_wdata[i*8 +: 8];
                        default: ;   // read-only or unused: ignore
                    endcase
        end
    end

    // ---------------- Read ----------------
    // Take the address (ARREADY for one clock), then present the data with
    // RVALID until the master takes it (RREADY).
    always @(posedge clk) begin
        if (rst) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 32'd0;
        end else begin
            if (!s_axi_arready && s_axi_arvalid && !s_axi_rvalid)
                s_axi_arready <= 1'b1;
            else
                s_axi_arready <= 1'b0;

            if (s_axi_arready && s_axi_arvalid) begin
                s_axi_rvalid <= 1'b1;
                case (s_axi_araddr[11:2])
                    10'h0:   s_axi_rdata <= reg0;
                    10'h1:   s_axi_rdata <= reg1;
                    10'h2:   s_axi_rdata <= sum;
                    10'h3:   s_axi_rdata <= count;
                    10'h4:   s_axi_rdata <= ID;
                    default: s_axi_rdata <= 32'd0;
                endcase
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule
