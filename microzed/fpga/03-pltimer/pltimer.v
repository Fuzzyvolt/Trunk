// pltimer: a periodic timer with an interrupt, on an AXI4-Lite bus (project 03).
//
// Offset  Name    Access  What it holds
// 0x00    CTRL    R/W     bit 0 EN: timer runs.  bit 1 IE: interrupt output enabled
// 0x04    PERIOD  R/W     clock cycles per tick (100 MHz: 100000 = 1 ms). 0 = no ticks
// 0x08    STATUS  R/W1C   bit 0 PENDING: a tick happened. Write 1 to clear it
// 0x0C    TICKS   R/W     ticks since reset (+1 per period). Any write sets it to 0
// 0x10    COUNT   R       position inside the current period (0 .. PERIOD-1)
// 0x14    ID      R       constant 0x4D5A0301 ("MZ", project 03, version 1)
// others          R       0 (writes are ignored)
//
// The CPU sees these at 0x43C10000 + offset (address set in build.tcl).
//
//   COUNT: 0,1,2 ... PERIOD-1, 0,1,2 ...      (only while EN = 1)
//                          |
//                          +-- tick: TICKS + 1, PENDING = 1
//
//   irq = PENDING and IE  -> IRQ_F2P[0] -> GIC (ID 61) -> Linux
//
// The interrupt is LEVEL-sensitive: irq stays high until the CPU writes 1 to
// STATUS bit 0. A handler that forgets to clear it is called again forever.
//
// The AXI4-Lite part is the same as axiregs.v (project 02); only the
// registers behind it are new.

`timescale 1ns / 1ps

module pltimer #(
    parameter [31:0] ID = 32'h4D5A_0301,
    parameter [31:0] PERIOD_RESET = 32'd100_000_000   // 1 s at 100 MHz
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
    input  wire        s_axi_rready,

    // Interrupt to the PS (IRQ_F2P). The attributes tell Vivado it is an
    // active-high level interrupt, which is what the GIC is set up for.
    (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 irq INTERRUPT" *)
    (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
    output reg         irq
);

    wire clk = s_axi_aclk;
    wire rst = ~s_axi_aresetn;

    assign s_axi_bresp = 2'b00;   // OKAY
    assign s_axi_rresp = 2'b00;   // OKAY

    localparam [9:0] A_CTRL   = 10'h0,   // byte offset 0x00
                     A_PERIOD = 10'h1,   // 0x04
                     A_STATUS = 10'h2,   // 0x08
                     A_TICKS  = 10'h3,   // 0x0C
                     A_COUNT  = 10'h4,   // 0x10
                     A_ID     = 10'h5;   // 0x14

    reg  [1:0]  ctrl;        // [0] EN, [1] IE
    reg  [31:0] period;
    reg  [31:0] count;
    reg  [31:0] ticks;
    reg         pending;

    wire en = ctrl[0];
    wire ie = ctrl[1];

    // ---------------- AXI write handshake (as in axiregs.v) ----------------
    wire wr_en = s_axi_awvalid && s_axi_awready && s_axi_wvalid && s_axi_wready;
    wire [9:0] wr_reg = s_axi_awaddr[11:2];

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

    // ---------------- The timer ----------------
    // tick is high for one clock at the end of each period.
    // ">=" (not "==") so that shrinking PERIOD below the current COUNT
    // ends the period at once instead of counting up to 2^32 first.
    wire tick = en && (period != 32'd0) && (count >= period - 32'd1);

    always @(posedge clk) begin
        if (rst || !en || tick)
            count <= 32'd0;
        else
            count <= count + 32'd1;
    end

    // Registers the CPU writes. A 32-bit CPU store sets all four WSTRB bits;
    // byte writes are honoured for PERIOD, the bit registers use byte 0.
    integer i;
    always @(posedge clk) begin
        if (rst) begin
            ctrl   <= 2'b00;
            period <= PERIOD_RESET;
        end else if (wr_en) begin
            case (wr_reg)
                A_CTRL:   if (s_axi_wstrb[0]) ctrl <= s_axi_wdata[1:0];
                A_PERIOD: for (i = 0; i < 4; i = i + 1)
                              if (s_axi_wstrb[i]) period[i*8 +: 8] <= s_axi_wdata[i*8 +: 8];
                default: ;
            endcase
        end
    end

    // TICKS: +1 per tick; any write to it sets it to 0.
    always @(posedge clk) begin
        if (rst || (wr_en && wr_reg == A_TICKS))
            ticks <= 32'd0;
        else if (tick)
            ticks <= ticks + 32'd1;
    end

    // PENDING: set by a tick, cleared by writing 1 to STATUS bit 0
    // ("write 1 to clear", W1C). If both happen in the same clock the tick
    // wins, so a tick is never lost.
    always @(posedge clk) begin
        if (rst)
            pending <= 1'b0;
        else if (tick)
            pending <= 1'b1;
        else if (wr_en && wr_reg == A_STATUS && s_axi_wstrb[0] && s_axi_wdata[0])
            pending <= 1'b0;
    end

    // The interrupt output comes straight from a flip-flop, so it never
    // glitches (an AND of two signals can spike for a moment when both change).
    always @(posedge clk) begin
        if (rst) irq <= 1'b0;
        else     irq <= pending && ie;
    end

    // ---------------- AXI read (as in axiregs.v) ----------------
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
                    A_CTRL:   s_axi_rdata <= {30'd0, ctrl};
                    A_PERIOD: s_axi_rdata <= period;
                    A_STATUS: s_axi_rdata <= {31'd0, pending};
                    A_TICKS:  s_axi_rdata <= ticks;
                    A_COUNT:  s_axi_rdata <= count;
                    A_ID:     s_axi_rdata <= ID;
                    default:  s_axi_rdata <= 32'd0;
                endcase
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

endmodule
