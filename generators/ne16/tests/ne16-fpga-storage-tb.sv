`timescale 1ns/1ps

module ne16_fpga_storage_tb #(
  parameter int WIDTH_FACTOR = 4,
  parameter bit READ_WRITE_COLLISIONS = 0
);
  logic clock = 0, reset_n = 0, clear = 1, test_mode = 0;
  logic write = 0, write_all = 0, read = 0;
  logic [2:0] waddr = 0, raddr = 0;
  logic [31:0] data = 0;
  logic [3:0] bytes = 0;
  logic [WIDTH_FACTOR-1:0] wide = 0;
  logic [WIDTH_FACTOR*32-1:0] wide_data = 0;
  logic [31:0] cfg_read, fifo_read, input_read, accum_read;
  logic [WIDTH_FACTOR*32-1:0] accum_wide;
  logic [7:0][31:0] cfg_words, input_words, accum_words;

  hwpe_ctrl_regfile_latch #(.ADDR_WIDTH(3)) cfg (
    .clk(clock), .rst_n(reset_n), .clear(clear),
    .ReadEnable(read), .ReadAddr(raddr), .ReadData(cfg_read),
    .WriteEnable(write), .WriteAddr(waddr), .WriteData(data), .WriteBE(bytes),
    .MemContent(cfg_words)
  );
  hwpe_stream_fifo_scm #(.ADDR_WIDTH(3)) fifo (
    .clk(clock), .rst_n(reset_n),
    .ReadEnable(read), .ReadAddr(raddr), .ReadData(fifo_read),
    .WriteEnable(write), .WriteAddr(waddr), .WriteData(data)
  );
  ne16_input_buffer_scm #(.ADDR_WIDTH(3), .NUM_WORDS(8), .DATA_WIDTH(32)) inputs (
    .clk_i(clock), .rst_ni(reset_n), .clear_i(clear), .test_mode_i(test_mode),
    .re_i(read), .raddr_i(raddr), .rdata_o(input_read),
    .we_i(write), .we_all_i(write_all), .waddr_i(waddr), .wdata_i(data),
    .input_buffer_o(input_words)
  );
  ne16_accumulator_scm #(.ADDR_WIDTH(3), .NUM_WORDS(8), .WIDTH_FACTOR(WIDTH_FACTOR)) accum (
    .clk_i(clock), .rst_ni(reset_n), .clear_i(clear), .test_mode_i(test_mode),
    .wide_enable_i(wide), .re_i(read), .raddr_i(raddr), .rdata_o(accum_read),
    .rdata_wide_o(accum_wide), .we_i(write), .we_all_i(write_all),
    .waddr_i(waddr), .wdata_i(data), .wdata_wide_i(wide_data),
    .accumulators_o(accum_words)
  );
  always #5 clock = ~clock;

  logic [7:0][31:0] expected_cfg = 0, expected_fifo = 0;
  logic [7:0][31:0] expected_input = 0, expected_accum = 0;
  logic [31:0] input_shadow = 0, expected_input_read = 0;
  logic [WIDTH_FACTOR-1:0][31:0] accum_shadow = 0, expected_accum_read = 0;
  logic [2:0] read_address = 0;
  int unsigned checked = 0;

  function automatic logic [31:0] selected_data(input int lane);
    case (wide)
      1, 2, 4, 8, 16, 32, 64, 128: return wide_data[0 +: 32];
      3, 12, 48, 192: return wide_data[(lane % 2)*32 +: 32];
      15, 240: return wide_data[(lane % 4)*32 +: 32];
      255: return wide_data[(lane % 8)*32 +: 32];
      default: return data;
    endcase
  endfunction

  task automatic step;
    logic selected;
    @(posedge clock);
    if (!reset_n || clear) begin
      expected_input_read = 0;
      expected_accum_read = 0;
    end else if (read) begin
      expected_input_read = expected_input[raddr];
      expected_accum_read[0] = expected_accum[raddr];
      if (|wide)
        for (int lane = 1; lane < WIDTH_FACTOR; lane++)
          expected_accum_read[lane] = expected_accum[(int'(raddr) + lane) % 8];
    end
    for (int word = 0; word < 8; word++) begin
      if (clear) begin
        expected_cfg[word] = 0;
        expected_input[word] = 0;
        expected_accum[word] = 0;
      end else begin
        if (write && int'(waddr) == word)
          for (int b = 0; b < 4; b++)
            if (bytes[b]) expected_cfg[word][b*8 +: 8] = data[b*8 +: 8];
        if (test_mode || write_all || (write && int'(waddr) == word))
          expected_input[word] = !reset_n ? 0 : write ? data : input_shadow;
        selected = |wide ?
            (int'(waddr) == (word / WIDTH_FACTOR)*WIDTH_FACTOR &&
             wide[word % WIDTH_FACTOR]) :
            int'(waddr) == word;
        if (test_mode || write_all || (write && selected))
          expected_accum[word] = !reset_n ? 0 :
              (write || write_all) ? selected_data(word % WIDTH_FACTOR) :
                                    accum_shadow[word % WIDTH_FACTOR];
      end
    end
    if (write) expected_fifo[waddr] = data;
    if (read) read_address = raddr;
    if (!reset_n || clear) begin
      input_shadow = 0;
      accum_shadow = 0;
    end else begin
      if (write) input_shadow = data;
      if (write || write_all)
        for (int lane = 0; lane < WIDTH_FACTOR; lane++)
          accum_shadow[lane] = selected_data(lane);
    end
    #1;
    if (cfg_words !== expected_cfg || cfg_read !== expected_cfg[read_address])
      $fatal(1, "Configuration storage mismatch at cycle %0d", checked);
    if (input_words !== expected_input || input_read !== expected_input_read)
      $fatal(1, "Input storage mismatch at cycle %0d: words=%h expected=%h read=%h expected=%h",
             checked, input_words, expected_input, input_read, expected_input_read);
    if (accum_words !== expected_accum ||
        accum_wide !== expected_accum_read || accum_read !== expected_accum_read[0])
      $fatal(1, "Accumulator storage mismatch at cycle %0d", checked);
    if (checked >= 8 && fifo_read !== expected_fifo[read_address])
      $fatal(1, "FIFO storage mismatch at cycle %0d", checked);
    checked++;
    @(negedge clock);
    #1;
  endtask

  initial begin
    int unsigned seed;
    seed = 32'hc014ab1e;
    seed = $urandom(seed);
    step();
    reset_n = 1;
    for (int word = 0; word < 8; word++) begin
      clear = 0;
      read = READ_WRITE_COLLISIONS;
      write = 1;
      bytes = 15;
      raddr = 3'(word);
      waddr = 3'(word);
      data = 32'h11223300 + 32'(word);
      step();
    end
    read = 0;
    for (int mask = 0; mask < 2**WIDTH_FACTOR; mask++) begin
      wide = WIDTH_FACTOR'(mask);
      waddr = 3'(WIDTH_FACTOR == 4 ? (mask % 2)*4 : 0);
      bytes = 4'(mask);
      data = 32'h80000000 + 32'(mask);
      for (int lane = 0; lane < WIDTH_FACTOR; lane++)
        wide_data[lane*32 +: 32] = 32'h12340000 + 32'(mask*16 + lane);
      write = 1;
      step();
      read = 1;
      write = 0;
      raddr = waddr;
      step();
      read = 0;
    end
    for (int cycle = 0; cycle < 1000; cycle++) begin
      reset_n = cycle != 50 && cycle != 500;
      clear = cycle % 71 == 0;
      test_mode = cycle % 37 == 0;
      write = 1'($urandom);
      write_all = cycle % 11 == 0;
      raddr = 3'($urandom);
      waddr = 3'($urandom);
      data = $urandom;
      bytes = 4'($urandom);
      wide = WIDTH_FACTOR'($urandom);
      for (int lane = 0; lane < WIDTH_FACTOR; lane++)
        wide_data[lane*32 +: 32] = $urandom;
      // Same-edge latch reads race with gate propagation in zero-delay simulation.
      read = 1'($urandom) &&
          (READ_WRITE_COLLISIONS || (!write && !write_all && !test_mode));
      step();
    end
    $display("STORAGE_PASS %0d cycles: masks, retention, clear/reset, broadcast, wide/read timing",
             checked);
    $finish;
  end
endmodule
