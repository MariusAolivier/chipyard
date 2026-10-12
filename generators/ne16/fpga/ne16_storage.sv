// FPGA equivalents of the pinned clock-gated SCMs. Keep their port contracts.
module hwpe_ctrl_regfile_latch #(
  parameter ADDR_WIDTH = 5,
  parameter DATA_WIDTH = 32,
  parameter NUM_BYTE = DATA_WIDTH / 8
) (
  input logic clk,
  input logic rst_n,
  input logic clear,
  input logic ReadEnable,
  input logic [ADDR_WIDTH-1:0] ReadAddr,
  output logic [DATA_WIDTH-1:0] ReadData,
  input logic WriteEnable,
  input logic [ADDR_WIDTH-1:0] WriteAddr,
  input logic [NUM_BYTE-1:0][7:0] WriteData,
  input logic [NUM_BYTE-1:0] WriteBE,
  output logic [2**ADDR_WIDTH-1:0][DATA_WIDTH-1:0] MemContent
);
  logic [ADDR_WIDTH-1:0] read_address;

  // As in the SCM, rst_n does not reset storage or the read-address register.
  always_ff @(posedge clk) begin
    if (ReadEnable) read_address <= ReadAddr;
    if (clear)
      MemContent <= '0;
    else if (WriteEnable)
      for (int b = 0; b < NUM_BYTE; b++)
        if (WriteBE[b])
          MemContent[WriteAddr][b*8 +: 8] <= WriteData[b];
  end
  assign ReadData = MemContent[read_address];
endmodule

module hwpe_stream_fifo_scm #(
  parameter ADDR_WIDTH = 5,
  parameter DATA_WIDTH = 32
) (
  input logic clk,
  input logic rst_n,
  input logic ReadEnable,
  input logic [ADDR_WIDTH-1:0] ReadAddr,
  output logic [DATA_WIDTH-1:0] ReadData,
  input logic WriteEnable,
  input logic [ADDR_WIDTH-1:0] WriteAddr,
  input logic [DATA_WIDTH-1:0] WriteData
);
  logic [ADDR_WIDTH-1:0] read_address;
  logic [2**ADDR_WIDTH-1:0][DATA_WIDTH-1:0] memory;

  // The FIFO wrapper resets validity/pointers; its SCM contents are not reset.
  always_ff @(posedge clk) begin
    if (ReadEnable) read_address <= ReadAddr;
    if (WriteEnable) memory[WriteAddr] <= WriteData;
  end
  assign ReadData = memory[read_address];
endmodule

module ne16_input_buffer_scm #(
  parameter int unsigned ADDR_WIDTH = 5,
  parameter int unsigned DATA_WIDTH = 128,
  parameter int unsigned NUM_WORDS = 25
) (
  input logic clk_i,
  input logic rst_ni,
  input logic clear_i,
  input logic test_mode_i,
  input logic re_i,
  input logic [ADDR_WIDTH-1:0] raddr_i,
  output logic [DATA_WIDTH-1:0] rdata_o,
  input logic we_i,
  input logic we_all_i,
  input logic [ADDR_WIDTH-1:0] waddr_i,
  input logic [DATA_WIDTH-1:0] wdata_i,
  output logic [NUM_WORDS-1:0][DATA_WIDTH-1:0] input_buffer_o
);
  logic [DATA_WIDTH-1:0] sampled_data;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      sampled_data <= '0;
      rdata_o <= '0;
    end else if (clear_i) begin
      sampled_data <= '0;
      rdata_o <= '0;
    end else begin
      if (we_i) sampled_data <= wdata_i;
      if (re_i) rdata_o <= input_buffer_o[raddr_i];
    end
  end

  for (genvar word = 0; word < NUM_WORDS; word++) begin : words
    always_ff @(posedge clk_i) begin
      if (clear_i)
        input_buffer_o[word] <= '0;
      else if (test_mode_i || we_all_i ||
               (we_i && waddr_i == ADDR_WIDTH'(word)))
        // A broadcast without we_i uses the previously sampled data.
        input_buffer_o[word] <= !rst_ni ? '0 :
                                we_i ? wdata_i : sampled_data;
    end
  end
endmodule

module ne16_accumulator_scm #(
  parameter int unsigned ADDR_WIDTH = 5,
  parameter int unsigned DATA_WIDTH = 32,
  parameter int unsigned NUM_WORDS = 2**ADDR_WIDTH,
  parameter int unsigned WIDTH_FACTOR = 4
) (
  input logic clk_i,
  input logic rst_ni,
  input logic clear_i,
  input logic test_mode_i,
  input logic [WIDTH_FACTOR-1:0] wide_enable_i,
  input logic re_i,
  input logic [ADDR_WIDTH-1:0] raddr_i,
  output logic [DATA_WIDTH-1:0] rdata_o,
  output logic [WIDTH_FACTOR*DATA_WIDTH-1:0] rdata_wide_o,
  input logic we_i,
  input logic we_all_i,
  input logic [ADDR_WIDTH-1:0] waddr_i,
  input logic [DATA_WIDTH-1:0] wdata_i,
  input logic [WIDTH_FACTOR*DATA_WIDTH-1:0] wdata_wide_i,
  output logic [NUM_WORDS-1:0][DATA_WIDTH-1:0] accumulators_o
);
  logic [WIDTH_FACTOR-1:0][DATA_WIDTH-1:0] selected_data, sampled_data;

  for (genvar lane = 0; lane < WIDTH_FACTOR; lane++) begin : lanes
    always_comb begin
      case (wide_enable_i)
        'h01, 'h02, 'h04, 'h08, 'h10, 'h20, 'h40, 'h80:
          selected_data[lane] = wdata_wide_i[0 +: DATA_WIDTH];
        'h03, 'h0c, 'h30, 'hc0:
          selected_data[lane] =
              wdata_wide_i[(lane % 2)*DATA_WIDTH +: DATA_WIDTH];
        'h0f, 'hf0:
          selected_data[lane] =
              wdata_wide_i[(lane % 4)*DATA_WIDTH +: DATA_WIDTH];
        'hff:
          selected_data[lane] =
              wdata_wide_i[(lane % 8)*DATA_WIDTH +: DATA_WIDTH];
        default: selected_data[lane] = wdata_i;
      endcase
    end

    wire [ADDR_WIDTH-1:0] read_address = raddr_i + ADDR_WIDTH'(lane);
    always_ff @(posedge clk_i or negedge rst_ni) begin
      if (!rst_ni) begin
        sampled_data[lane] <= '0;
        rdata_wide_o[lane*DATA_WIDTH +: DATA_WIDTH] <= '0;
      end else if (clear_i) begin
        sampled_data[lane] <= '0;
        rdata_wide_o[lane*DATA_WIDTH +: DATA_WIDTH] <= '0;
      end else begin
        if (we_i || we_all_i) sampled_data[lane] <= selected_data[lane];
        if (re_i && (lane == 0 || |wide_enable_i))
          rdata_wide_o[lane*DATA_WIDTH +: DATA_WIDTH] <=
              accumulators_o[read_address];
      end
    end
  end
  assign rdata_o = rdata_wide_o[0 +: DATA_WIDTH];

  for (genvar word = 0; word < NUM_WORDS; word++) begin : words
    wire selected_word = |wide_enable_i ?
        (waddr_i == ADDR_WIDTH'((word / WIDTH_FACTOR)*WIDTH_FACTOR) &&
         wide_enable_i[word % WIDTH_FACTOR]) :
        waddr_i == ADDR_WIDTH'(word);
    always_ff @(posedge clk_i) begin
      if (clear_i)
        accumulators_o[word] <= '0;
      else if (test_mode_i || we_all_i || (we_i && selected_word))
        accumulators_o[word] <= !rst_ni ? '0 :
            (we_i || we_all_i) ? selected_data[word % WIDTH_FACTOR] :
                                sampled_data[word % WIDTH_FACTOR];
    end
  end
endmodule
