`timescale 1ns/1ps

module ne16_fpga_core_tb;
  logic clock = 0;
  logic reset_n = 0;
  logic [1:0] evt_o;
  logic busy_o;
  logic [8:0] tcdm_req_o, tcdm_gnt_i, tcdm_wen_o, tcdm_r_valid_i = 0;
  logic [287:0] tcdm_add_o, tcdm_data_o, tcdm_r_data_i = 0;
  logic [35:0] tcdm_be_o;
  logic periph_req_i = 0, periph_gnt_o, periph_wen_i = 0;
  logic [31:0] periph_add_i = 0, periph_data_i = 0, periph_r_data_o;
  logic [3:0] periph_be_i = 4'hf;
  logic periph_r_valid_o;
  byte unsigned memory [65536];
  byte unsigned expected_memory [65536];
  byte unsigned fixture_input [1024], fixture_weights [2432], fixture_output [1024];
  int unsigned grants = 0, stores = 0;
  int unsigned cycles = 0;
  bit conv3x3, backpressure, dory_fixture;
  string fixture_dir;
  bit wrap_invalid_address, invalid_address_reported = 0;
  localparam logic [31:0] BASE = 32'h20000000;

  NE16BlackBox dut (.*);
  always #5 clock = ~clock;
  assign tcdm_gnt_i =
      {9{reset_n && |tcdm_req_o && (!backpressure || cycles % 3 != 0)}};

  always @(posedge clock) begin
    tcdm_r_valid_i <= 0;
    cycles <= cycles + 1;
    if (reset_n && |tcdm_req_o && &tcdm_gnt_i) begin
      if (tcdm_req_o != 9'h1ff ||
          tcdm_wen_o != {9{tcdm_wen_o[0]}})
        $fatal(1, "Non-atomic TCDM request");
      grants <= grants + 1;
      for (int lane = 0; lane < 9; lane++) begin
        if (tcdm_add_o[lane*32 +: 32] != tcdm_add_o[31:0] + 4*lane)
          $fatal(1, "Nonconsecutive TCDM addresses");
        if (tcdm_add_o[lane*32 +: 32] < BASE ||
            tcdm_add_o[lane*32 +: 32] > BASE + 65532) begin
          if (!wrap_invalid_address)
            $fatal(1, "Invalid TCDM address: lane=%0d address=%08x",
                   lane, tcdm_add_o[lane*32 +: 32]);
          if (!invalid_address_reported) begin
            $display("WRAPPED_INVALID_TCDM address=%08x",
                     tcdm_add_o[lane*32 +: 32]);
            invalid_address_reported = 1;
          end
        end
        for (int b = 0; b < 4; b++) begin
          if (tcdm_wen_o[0])
            tcdm_r_data_i[lane*32 + b*8 +: 8] <=
                memory[(tcdm_add_o[lane*32 +: 32] - BASE + b) & 'hffff];
          else if (tcdm_be_o[lane*4 + b])
            memory[(tcdm_add_o[lane*32 +: 32] - BASE + b) & 'hffff] <=
                tcdm_data_o[lane*32 + b*8 +: 8];
        end
      end
      if (tcdm_wen_o[0])
        tcdm_r_valid_i <= 9'h1ff;
      else
        stores <= stores + 1;
    end
  end

  task automatic access(input bit read, input logic [31:0] address,
                        input logic [31:0] data,
                        output logic [31:0] result);
    bit accepted;
    @(negedge clock);
    periph_add_i = address;
    periph_data_i = data;
    periph_wen_i = read;
    periph_req_i = 1;
    accepted = 0;
    for (int cycle = 0; cycle < 128; cycle++) begin
      @(posedge clock);
      if (periph_gnt_o) begin
        accepted = 1;
        break;
      end
    end
    if (!accepted) $fatal(1, "Control grant timeout at %08x", address);
    @(negedge clock);
    periph_req_i = 0;
    result = 0;
    if (read) begin
      for (int cycle = 0; cycle < 128; cycle++) begin
        if (periph_r_valid_o) begin
          result = periph_r_data_o;
          break;
        end
        if (cycle == 127)
          $fatal(1, "Control response timeout at %08x", address);
        @(negedge clock);
      end
    end
    @(negedge clock);
  endtask

  task automatic submit(input int unsigned job);
    logic [31:0] words [24];
    logic [31:0] value;
    words = '{BASE + 'h1000, BASE + job*'h400,
              BASE + 'h2000 + job*'h1000, 0, 0, 0,
              16, 48, 0, 32, 4, 12, 16, 16, 0,
              'h00010010, 'h00030003, 'h00030003, 'h00010001,
              'h00010001, 0, 'hffffff80, 0, 'h00408047};
    if (conv3x3) begin
      words[7] = 80;
      words[12] = 18;
      words[13] = 144;
      words[17] = 'h00050005;
      words[23] = 'h00408007;
    end
    if (dory_fixture) begin
      words[0] = BASE + 'h2800 + job*'h5000;
      words[1] = BASE + 'h1800 + job*'h5000 - 144;
      words[2] = BASE + 'h2000 + job*'h5000;
      words[3] = BASE + 'h3100 + job*'h5000;
      words[5] = BASE + 'h3140 + job*'h5000;
      words[7] = 128;
      words[10] = 16;
      words[11] = 128;
      words[12] = 18;
      words[13] = 144;
      words[15] = 'h00100010;
      words[16] = 'h00020002;
      words[17] = 'h00030003;
      words[19] = 'h00030003;
      words[20] = 'h11110000;
      words[23] = 'h020ba017;
    end
    access(1, 'h04, 0, value);
    if (value >= 'h101) $fatal(1, "Acquire failed: %08x", value);
    for (int word = 0; word < 24; word++)
      access(0, 'h20 + 4*word, words[word], value);
    for (int word = 0; word < 24; word++) begin
      access(1, 'h20 + 4*word, 0, value);
      if (word < 2)
        $display("CONFIG_READBACK job=%0d word=%0d actual=%08x expected=%08x",
                 job, word, value, words[word]);
      if (!wrap_invalid_address && value != words[word])
        $fatal(1, "Configuration mismatch job=%0d word=%0d", job, word);
    end
    access(0, 'h00, 0, value);
  endtask

  task automatic wait_idle;
    logic [31:0] value;
    for (int poll = 0; poll < 20000; poll++) begin
      access(1, 'h0c, 0, value);
      if (value == 0) return;
    end
    $fatal(1, "CORE_TIMEOUT status=%08x grants=%0d stores=%0d",
           value, grants, stores);
  endtask

  initial begin
    logic [31:0] value;
    logic [31:0] expected;
    // Hardware drops high address bits; strict mode catches the first bad pointer.
    wrap_invalid_address = $test$plusargs("wrap-invalid-address");
    conv3x3 = $test$plusargs("conv3x3");
    backpressure = $test$plusargs("backpressure");
    dory_fixture = $test$plusargs("dory-fixture");
    if (conv3x3 && dory_fixture) $fatal(1, "Select only one convolution fixture");
    foreach (memory[i]) memory[i] = 0;
    for (int i = 0; i < (conv3x3 ? 400 : 144); i++) begin
      memory[i] = conv3x3 ? 8'(1 + i % 13) : 1;
      memory['h400 + i] = conv3x3 ? 8'(2 + i % 7) : 2;
    end
    memory['h1000] = 'hff;
    memory['h1001] = 'hff;
    memory['h100e] = 'hff;
    memory['h100f] = 'hff;
    if (conv3x3)
      for (int spatial = 0; spatial < 9; spatial++) begin
        memory['h1000 + spatial*2] = 'hff;
        memory['h1001 + spatial*2] = 'hff;
        memory['h1000 + (63 + spatial)*2] = 'hff;
        memory['h1001 + (63 + spatial)*2] = 'hff;
      end
    for (int job = 0; job < 2; job++)
      for (int pixel = 0; pixel < 9; pixel++) begin
        memory['h2000 + job*'h1000 + 4*pixel] = 'hef;
        memory['h2001 + job*'h1000 + 4*pixel] = 'hbe;
        memory['h2002 + job*'h1000 + 4*pixel] = 'had;
        memory['h2003 + job*'h1000 + 4*pixel] = 'hde;
      end
    if (dory_fixture) begin
      if (!$value$plusargs("fixture-dir=%s", fixture_dir))
        $fatal(1, "DORY fixture directory is required");
      $readmemh({fixture_dir, "/input.hex"}, fixture_input);
      $readmemh({fixture_dir, "/weights.hex"}, fixture_weights);
      $readmemh({fixture_dir, "/expected.hex"}, fixture_output);
      foreach (memory[i]) memory[i] = 'ha5;
      // Match the disjoint guarded slots used by ne16-double-buffer.c.
      for (int job = 0; job < 2; job++) begin
        for (int i = 0; i < 1024; i++)
          memory['h1800 + job*'h5000 + i] = fixture_input[i];
        for (int i = 0; i < 2432; i++)
          memory['h2800 + job*'h5000 + i] = fixture_weights[i];
      end
    end
    foreach (memory[i]) expected_memory[i] = memory[i];
    repeat (8) @(negedge clock);
    reset_n = 1;
    access(0, 'h14, 0, value);
    submit(0);
    if (dory_fixture) wait_idle();
    submit(1);
    wait_idle();
    for (int job = 0; job < 2; job++) begin
      if (dory_fixture) begin
        for (int i = 0; i < 1024; i++)
          expected_memory['h2000 + job*'h5000 + i] = fixture_output[i];
      end else for (int pixel = 0; pixel < 9; pixel++) begin
        value = {memory['h2003 + job*'h1000 + 4*pixel],
                 memory['h2002 + job*'h1000 + 4*pixel],
                 memory['h2001 + job*'h1000 + 4*pixel],
                 memory['h2000 + job*'h1000 + 4*pixel]};
        expected = 16*(job + 1);
        if (conv3x3) begin
          expected = 0;
          for (int ky = 0; ky < 3; ky++)
            for (int kx = 0; kx < 3; kx++)
              for (int channel = 0; channel < 16; channel++)
                expected += 32'(memory[job*'h400 +
                    (((pixel / 3 + ky)*5 + pixel % 3 + kx)*16 + channel)]);
        end
        if (value != expected)
          $fatal(1, "CORE_MISMATCH job=%0d pixel=%0d actual=%08x expected=%0d",
                 job, pixel, value, expected);
        for (int b = 0; b < 4; b++)
          expected_memory['h2000 + job*'h1000 + pixel*4 + b] =
              expected[b*8 +: 8];
      end
    end
    foreach (memory[i])
      if (memory[i] != expected_memory[i])
        $fatal(1, "Output/source/guard mismatch at %04x: actual=%02x expected=%02x",
               i, memory[i], expected_memory[i]);
    $display("CORE_PASS two %s convolutions grants=%0d stores=%0d",
             dory_fixture ? "serialized quantized" : "queued", grants, stores);
    $finish;
  end
endmodule
