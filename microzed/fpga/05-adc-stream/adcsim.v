// adcsim: a simulated ADC (project 05). Makes one sample every DIV clocks and
// sends it out as an AXI4-Stream, with TLAST every PKT_LEN samples, so the
// AXI DMA (S2MM) writes one packet into one DDR buffer.
//
// Offset  Name       Access  What it holds
// 0x00    CTRL       R/W     bit 0 EN: run.  bit 1 SINE: 1 = sine, 0 = sawtooth.
//                            bit 2 FLUSH: while 1, the stream FIFO is held in reset
//                            and the generator is stopped at once
// 0x04    STATUS     R       bit 0 RUNNING (stays 1 after EN=0 until the packet ends)
// 0x08    DIV        R/W     clock cycles per sample (100 MHz: 100 = 1 MSa/s). 0 counts as 1
// 0x0C    PKT_LEN    R/W     samples per packet (TLAST on the last one). 0 counts as 1
// 0x10    PHASE_INC  R/W     added to the 32-bit phase every sample:
//                            signal frequency = sample rate * PHASE_INC / 2^32
// 0x14    SAMPLES    R       samples made since the last start (sent + dropped)
// 0x18    DROPPED    R       samples dropped since the last start (stream not ready)
// 0x1C    PACKETS    R       packets sent since the last start (TLAST handshakes)
// 0x20    ID         R       0x4D5A0501 ("MZ", project 05, version 1)
//
// The CPU sees these at 0x43C20000 + offset (address set in build.tcl).
//
// Sample word (32 bits):  [31:16] sequence number (+1 per sample, wraps)
//                         [15:0]  value, signed 16 bit (-32768 .. 32767)
// The sequence number counts dropped samples too, so a gap in it shows exactly
// how many samples were lost, and where.
//
//          every DIV clocks
//   phase ----------------> value (sine table or sawtooth) --> m_axis --> FIFO --> DMA
//   phase += PHASE_INC                                        TLAST every PKT_LEN
//
// A real ADC cannot wait: if the stream is not ready when the next sample is
// due (FIFO full because the DMA stopped), that sample is DROPPED and counted.
//
// Start: EN 0 -> 1 clears the counters, the phase and the sequence number.
// Stop:  EN 1 -> 0 finishes the current packet first (so the DMA buffer in
//        flight gets its TLAST), then RUNNING goes to 0. If the packet cannot
//        finish (DMA stopped), FLUSH ends it.

`timescale 1ns / 1ps

module adcsim #(
    parameter [31:0] ID = 32'h4D5A_0501
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s_axi_aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "ASSOCIATED_BUSIF s_axi:m_axis, ASSOCIATED_RESET s_axi_aresetn" *)
    input  wire        s_axi_aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 s_axi_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        s_axi_aresetn,

    // AXI4-Lite slave (registers), same as axiregs.v / pltimer.v
    input  wire [11:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output reg         s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output reg         s_axi_wready,
    output wire [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [11:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output reg         s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output wire [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,

    // AXI4-Stream master (samples). The m_axis_* names make Vivado see one
    // stream interface. A word moves when TVALID and TREADY are both 1.
    output reg  [31:0] m_axis_tdata,
    output wire [3:0]  m_axis_tkeep,
    output reg         m_axis_tlast,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,

    // Reset for the stream FIFO, so FLUSH can empty it (active low).
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 fifo_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    output reg         fifo_aresetn
);

    wire clk = s_axi_aclk;
    wire rst = ~s_axi_aresetn;

    assign s_axi_bresp  = 2'b00;   // OKAY
    assign s_axi_rresp  = 2'b00;   // OKAY
    assign m_axis_tkeep = 4'hF;    // all 4 bytes of every word are valid

    localparam [9:0] A_CTRL      = 10'h0,   // 0x00
                     A_STATUS    = 10'h1,   // 0x04
                     A_DIV       = 10'h2,   // 0x08
                     A_PKT_LEN   = 10'h3,   // 0x0C
                     A_PHASE_INC = 10'h4,   // 0x10
                     A_SAMPLES   = 10'h5,   // 0x14
                     A_DROPPED   = 10'h6,   // 0x18
                     A_PACKETS   = 10'h7,   // 0x1C
                     A_ID        = 10'h8;   // 0x20

    // ---------------- Registers the CPU writes ----------------
    reg  [2:0]  ctrl;        // [0] EN, [1] SINE, [2] FLUSH
    reg  [31:0] div;
    reg  [31:0] pkt_len;
    reg  [31:0] phase_inc;

    wire en    = ctrl[0];
    wire sine  = ctrl[1];
    wire flush = ctrl[2];

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

    // Full 32-bit writes only (the driver always uses writel).
    always @(posedge clk) begin
        if (rst) begin
            ctrl      <= 3'b000;
            div       <= 32'd100;          // 1 MSa/s at 100 MHz
            pkt_len   <= 32'd4096;
            phase_inc <= 32'd4294967;      // 2^32 / 1000: 1 kHz at 1 MSa/s
        end else if (wr_en) begin
            case (wr_reg)
                A_CTRL:      ctrl      <= s_axi_wdata[2:0];
                A_DIV:       div       <= s_axi_wdata;
                A_PKT_LEN:   pkt_len   <= s_axi_wdata;
                A_PHASE_INC: phase_inc <= s_axi_wdata;
                default: ;
            endcase
        end
    end

    // ---------------- The generator ----------------
    reg         running;
    reg  [31:0] divcnt;      // clocks since the last sample
    reg  [31:0] pktcnt;      // samples sent in the current packet
    reg  [31:0] phase;
    reg  [15:0] seq;
    reg  [31:0] samples, dropped, packets;

    wire [31:0] div_m1 = (div     == 32'd0) ? 32'd0 : div     - 32'd1;
    wire [31:0] len_m1 = (pkt_len == 32'd0) ? 32'd0 : pkt_len - 32'd1;

    wire start  = en && !running;
    // A sample is due once every DIV clocks. ">=" so that lowering DIV takes
    // effect at once.
    wire due    = running && (divcnt >= div_m1);
    // Make it while EN=1, or after EN=0 until the current packet is complete.
    wire make   = due && (en || pktcnt != 32'd0);
    // The output register is free if it is empty or being emptied right now.
    wire room   = !m_axis_tvalid || m_axis_tready;

    // Value from the phase. Sawtooth: top 16 bits with the sign bit flipped,
    // so it ramps from -32768 to 32767. Sine: table of 256 points, below.
    reg signed [15:0] sine_v;
    wire       [15:0] saw_v = {~phase[31], phase[30:16]};
    wire       [15:0] value = sine ? sine_v : saw_v;

    always @(posedge clk) begin
        if (rst || flush) begin
            running       <= 1'b0;
            divcnt        <= 32'd0;
            pktcnt        <= 32'd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tlast  <= 1'b0;
            if (rst) begin
                m_axis_tdata <= 32'd0;
                phase   <= 32'd0;
                seq     <= 16'd0;
                samples <= 32'd0;
                dropped <= 32'd0;
                packets <= 32'd0;
            end
        end else if (start) begin
            running <= 1'b1;
            divcnt  <= 32'd0;
            pktcnt  <= 32'd0;
            phase   <= 32'd0;
            seq     <= 16'd0;
            samples <= 32'd0;
            dropped <= 32'd0;
            packets <= 32'd0;
        end else begin
            // The DMA side took the word in the output register.
            if (m_axis_tvalid && m_axis_tready) begin
                m_axis_tvalid <= 1'b0;
                if (m_axis_tlast)
                    packets <= packets + 32'd1;
            end

            if (running)
                divcnt <= due ? 32'd0 : divcnt + 32'd1;

            if (make) begin
                samples <= samples + 32'd1;
                seq     <= seq + 16'd1;
                phase   <= phase + phase_inc;
                if (room) begin
                    m_axis_tdata  <= {seq, value};
                    m_axis_tvalid <= 1'b1;
                    m_axis_tlast  <= (pktcnt >= len_m1);
                    pktcnt        <= (pktcnt >= len_m1) ? 32'd0 : pktcnt + 32'd1;
                end else begin
                    dropped <= dropped + 32'd1;   // too late: this sample is lost
                end
            end

            // Stopped, packet complete, last word gone: done.
            if (running && !en && pktcnt == 32'd0 && !m_axis_tvalid)
                running <= 1'b0;
        end
    end

    // FIFO reset from a flip-flop: low during reset and while FLUSH is 1.
    always @(posedge clk)
        fifo_aresetn <= !(rst || flush);

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
                    A_CTRL:      s_axi_rdata <= {29'd0, ctrl};
                    A_STATUS:    s_axi_rdata <= {31'd0, running};
                    A_DIV:       s_axi_rdata <= div;
                    A_PKT_LEN:   s_axi_rdata <= pkt_len;
                    A_PHASE_INC: s_axi_rdata <= phase_inc;
                    A_SAMPLES:   s_axi_rdata <= samples;
                    A_DROPPED:   s_axi_rdata <= dropped;
                    A_PACKETS:   s_axi_rdata <= packets;
                    A_ID:        s_axi_rdata <= ID;
                    default:     s_axi_rdata <= 32'd0;
                endcase
            end else if (s_axi_rvalid && s_axi_rready) begin
                s_axi_rvalid <= 1'b0;
            end
        end
    end

    // ---------------- Sine table ----------------
    // round(32767 * sin(2 * pi * i / 256)), i = phase[31:24].
    // Synthesis turns this into a small ROM (LUTs). Made with Python:
    //   for i in range(256): print(i, round(32767*math.sin(2*math.pi*i/256)))
    always @(*) begin
        case (phase[31:24])
            8'd0  : sine_v =  16'sd0;
            8'd1  : sine_v =  16'sd804;
            8'd2  : sine_v =  16'sd1608;
            8'd3  : sine_v =  16'sd2410;
            8'd4  : sine_v =  16'sd3212;
            8'd5  : sine_v =  16'sd4011;
            8'd6  : sine_v =  16'sd4808;
            8'd7  : sine_v =  16'sd5602;
            8'd8  : sine_v =  16'sd6393;
            8'd9  : sine_v =  16'sd7179;
            8'd10 : sine_v =  16'sd7962;
            8'd11 : sine_v =  16'sd8739;
            8'd12 : sine_v =  16'sd9512;
            8'd13 : sine_v =  16'sd10278;
            8'd14 : sine_v =  16'sd11039;
            8'd15 : sine_v =  16'sd11793;
            8'd16 : sine_v =  16'sd12539;
            8'd17 : sine_v =  16'sd13279;
            8'd18 : sine_v =  16'sd14010;
            8'd19 : sine_v =  16'sd14732;
            8'd20 : sine_v =  16'sd15446;
            8'd21 : sine_v =  16'sd16151;
            8'd22 : sine_v =  16'sd16846;
            8'd23 : sine_v =  16'sd17530;
            8'd24 : sine_v =  16'sd18204;
            8'd25 : sine_v =  16'sd18868;
            8'd26 : sine_v =  16'sd19519;
            8'd27 : sine_v =  16'sd20159;
            8'd28 : sine_v =  16'sd20787;
            8'd29 : sine_v =  16'sd21403;
            8'd30 : sine_v =  16'sd22005;
            8'd31 : sine_v =  16'sd22594;
            8'd32 : sine_v =  16'sd23170;
            8'd33 : sine_v =  16'sd23731;
            8'd34 : sine_v =  16'sd24279;
            8'd35 : sine_v =  16'sd24811;
            8'd36 : sine_v =  16'sd25329;
            8'd37 : sine_v =  16'sd25832;
            8'd38 : sine_v =  16'sd26319;
            8'd39 : sine_v =  16'sd26790;
            8'd40 : sine_v =  16'sd27245;
            8'd41 : sine_v =  16'sd27683;
            8'd42 : sine_v =  16'sd28105;
            8'd43 : sine_v =  16'sd28510;
            8'd44 : sine_v =  16'sd28898;
            8'd45 : sine_v =  16'sd29268;
            8'd46 : sine_v =  16'sd29621;
            8'd47 : sine_v =  16'sd29956;
            8'd48 : sine_v =  16'sd30273;
            8'd49 : sine_v =  16'sd30571;
            8'd50 : sine_v =  16'sd30852;
            8'd51 : sine_v =  16'sd31113;
            8'd52 : sine_v =  16'sd31356;
            8'd53 : sine_v =  16'sd31580;
            8'd54 : sine_v =  16'sd31785;
            8'd55 : sine_v =  16'sd31971;
            8'd56 : sine_v =  16'sd32137;
            8'd57 : sine_v =  16'sd32285;
            8'd58 : sine_v =  16'sd32412;
            8'd59 : sine_v =  16'sd32521;
            8'd60 : sine_v =  16'sd32609;
            8'd61 : sine_v =  16'sd32678;
            8'd62 : sine_v =  16'sd32728;
            8'd63 : sine_v =  16'sd32757;
            8'd64 : sine_v =  16'sd32767;
            8'd65 : sine_v =  16'sd32757;
            8'd66 : sine_v =  16'sd32728;
            8'd67 : sine_v =  16'sd32678;
            8'd68 : sine_v =  16'sd32609;
            8'd69 : sine_v =  16'sd32521;
            8'd70 : sine_v =  16'sd32412;
            8'd71 : sine_v =  16'sd32285;
            8'd72 : sine_v =  16'sd32137;
            8'd73 : sine_v =  16'sd31971;
            8'd74 : sine_v =  16'sd31785;
            8'd75 : sine_v =  16'sd31580;
            8'd76 : sine_v =  16'sd31356;
            8'd77 : sine_v =  16'sd31113;
            8'd78 : sine_v =  16'sd30852;
            8'd79 : sine_v =  16'sd30571;
            8'd80 : sine_v =  16'sd30273;
            8'd81 : sine_v =  16'sd29956;
            8'd82 : sine_v =  16'sd29621;
            8'd83 : sine_v =  16'sd29268;
            8'd84 : sine_v =  16'sd28898;
            8'd85 : sine_v =  16'sd28510;
            8'd86 : sine_v =  16'sd28105;
            8'd87 : sine_v =  16'sd27683;
            8'd88 : sine_v =  16'sd27245;
            8'd89 : sine_v =  16'sd26790;
            8'd90 : sine_v =  16'sd26319;
            8'd91 : sine_v =  16'sd25832;
            8'd92 : sine_v =  16'sd25329;
            8'd93 : sine_v =  16'sd24811;
            8'd94 : sine_v =  16'sd24279;
            8'd95 : sine_v =  16'sd23731;
            8'd96 : sine_v =  16'sd23170;
            8'd97 : sine_v =  16'sd22594;
            8'd98 : sine_v =  16'sd22005;
            8'd99 : sine_v =  16'sd21403;
            8'd100: sine_v =  16'sd20787;
            8'd101: sine_v =  16'sd20159;
            8'd102: sine_v =  16'sd19519;
            8'd103: sine_v =  16'sd18868;
            8'd104: sine_v =  16'sd18204;
            8'd105: sine_v =  16'sd17530;
            8'd106: sine_v =  16'sd16846;
            8'd107: sine_v =  16'sd16151;
            8'd108: sine_v =  16'sd15446;
            8'd109: sine_v =  16'sd14732;
            8'd110: sine_v =  16'sd14010;
            8'd111: sine_v =  16'sd13279;
            8'd112: sine_v =  16'sd12539;
            8'd113: sine_v =  16'sd11793;
            8'd114: sine_v =  16'sd11039;
            8'd115: sine_v =  16'sd10278;
            8'd116: sine_v =  16'sd9512;
            8'd117: sine_v =  16'sd8739;
            8'd118: sine_v =  16'sd7962;
            8'd119: sine_v =  16'sd7179;
            8'd120: sine_v =  16'sd6393;
            8'd121: sine_v =  16'sd5602;
            8'd122: sine_v =  16'sd4808;
            8'd123: sine_v =  16'sd4011;
            8'd124: sine_v =  16'sd3212;
            8'd125: sine_v =  16'sd2410;
            8'd126: sine_v =  16'sd1608;
            8'd127: sine_v =  16'sd804;
            8'd128: sine_v =  16'sd0;
            8'd129: sine_v = -16'sd804;
            8'd130: sine_v = -16'sd1608;
            8'd131: sine_v = -16'sd2410;
            8'd132: sine_v = -16'sd3212;
            8'd133: sine_v = -16'sd4011;
            8'd134: sine_v = -16'sd4808;
            8'd135: sine_v = -16'sd5602;
            8'd136: sine_v = -16'sd6393;
            8'd137: sine_v = -16'sd7179;
            8'd138: sine_v = -16'sd7962;
            8'd139: sine_v = -16'sd8739;
            8'd140: sine_v = -16'sd9512;
            8'd141: sine_v = -16'sd10278;
            8'd142: sine_v = -16'sd11039;
            8'd143: sine_v = -16'sd11793;
            8'd144: sine_v = -16'sd12539;
            8'd145: sine_v = -16'sd13279;
            8'd146: sine_v = -16'sd14010;
            8'd147: sine_v = -16'sd14732;
            8'd148: sine_v = -16'sd15446;
            8'd149: sine_v = -16'sd16151;
            8'd150: sine_v = -16'sd16846;
            8'd151: sine_v = -16'sd17530;
            8'd152: sine_v = -16'sd18204;
            8'd153: sine_v = -16'sd18868;
            8'd154: sine_v = -16'sd19519;
            8'd155: sine_v = -16'sd20159;
            8'd156: sine_v = -16'sd20787;
            8'd157: sine_v = -16'sd21403;
            8'd158: sine_v = -16'sd22005;
            8'd159: sine_v = -16'sd22594;
            8'd160: sine_v = -16'sd23170;
            8'd161: sine_v = -16'sd23731;
            8'd162: sine_v = -16'sd24279;
            8'd163: sine_v = -16'sd24811;
            8'd164: sine_v = -16'sd25329;
            8'd165: sine_v = -16'sd25832;
            8'd166: sine_v = -16'sd26319;
            8'd167: sine_v = -16'sd26790;
            8'd168: sine_v = -16'sd27245;
            8'd169: sine_v = -16'sd27683;
            8'd170: sine_v = -16'sd28105;
            8'd171: sine_v = -16'sd28510;
            8'd172: sine_v = -16'sd28898;
            8'd173: sine_v = -16'sd29268;
            8'd174: sine_v = -16'sd29621;
            8'd175: sine_v = -16'sd29956;
            8'd176: sine_v = -16'sd30273;
            8'd177: sine_v = -16'sd30571;
            8'd178: sine_v = -16'sd30852;
            8'd179: sine_v = -16'sd31113;
            8'd180: sine_v = -16'sd31356;
            8'd181: sine_v = -16'sd31580;
            8'd182: sine_v = -16'sd31785;
            8'd183: sine_v = -16'sd31971;
            8'd184: sine_v = -16'sd32137;
            8'd185: sine_v = -16'sd32285;
            8'd186: sine_v = -16'sd32412;
            8'd187: sine_v = -16'sd32521;
            8'd188: sine_v = -16'sd32609;
            8'd189: sine_v = -16'sd32678;
            8'd190: sine_v = -16'sd32728;
            8'd191: sine_v = -16'sd32757;
            8'd192: sine_v = -16'sd32767;
            8'd193: sine_v = -16'sd32757;
            8'd194: sine_v = -16'sd32728;
            8'd195: sine_v = -16'sd32678;
            8'd196: sine_v = -16'sd32609;
            8'd197: sine_v = -16'sd32521;
            8'd198: sine_v = -16'sd32412;
            8'd199: sine_v = -16'sd32285;
            8'd200: sine_v = -16'sd32137;
            8'd201: sine_v = -16'sd31971;
            8'd202: sine_v = -16'sd31785;
            8'd203: sine_v = -16'sd31580;
            8'd204: sine_v = -16'sd31356;
            8'd205: sine_v = -16'sd31113;
            8'd206: sine_v = -16'sd30852;
            8'd207: sine_v = -16'sd30571;
            8'd208: sine_v = -16'sd30273;
            8'd209: sine_v = -16'sd29956;
            8'd210: sine_v = -16'sd29621;
            8'd211: sine_v = -16'sd29268;
            8'd212: sine_v = -16'sd28898;
            8'd213: sine_v = -16'sd28510;
            8'd214: sine_v = -16'sd28105;
            8'd215: sine_v = -16'sd27683;
            8'd216: sine_v = -16'sd27245;
            8'd217: sine_v = -16'sd26790;
            8'd218: sine_v = -16'sd26319;
            8'd219: sine_v = -16'sd25832;
            8'd220: sine_v = -16'sd25329;
            8'd221: sine_v = -16'sd24811;
            8'd222: sine_v = -16'sd24279;
            8'd223: sine_v = -16'sd23731;
            8'd224: sine_v = -16'sd23170;
            8'd225: sine_v = -16'sd22594;
            8'd226: sine_v = -16'sd22005;
            8'd227: sine_v = -16'sd21403;
            8'd228: sine_v = -16'sd20787;
            8'd229: sine_v = -16'sd20159;
            8'd230: sine_v = -16'sd19519;
            8'd231: sine_v = -16'sd18868;
            8'd232: sine_v = -16'sd18204;
            8'd233: sine_v = -16'sd17530;
            8'd234: sine_v = -16'sd16846;
            8'd235: sine_v = -16'sd16151;
            8'd236: sine_v = -16'sd15446;
            8'd237: sine_v = -16'sd14732;
            8'd238: sine_v = -16'sd14010;
            8'd239: sine_v = -16'sd13279;
            8'd240: sine_v = -16'sd12539;
            8'd241: sine_v = -16'sd11793;
            8'd242: sine_v = -16'sd11039;
            8'd243: sine_v = -16'sd10278;
            8'd244: sine_v = -16'sd9512;
            8'd245: sine_v = -16'sd8739;
            8'd246: sine_v = -16'sd7962;
            8'd247: sine_v = -16'sd7179;
            8'd248: sine_v = -16'sd6393;
            8'd249: sine_v = -16'sd5602;
            8'd250: sine_v = -16'sd4808;
            8'd251: sine_v = -16'sd4011;
            8'd252: sine_v = -16'sd3212;
            8'd253: sine_v = -16'sd2410;
            8'd254: sine_v = -16'sd1608;
            8'd255: sine_v = -16'sd804;
        endcase
    end

endmodule
