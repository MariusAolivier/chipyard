module ita_hwpe_smoke_tb;
  timeunit 1ns;
  timeprecision 1ps;

  localparam int unsigned MemDataWidth = 64;
  localparam int unsigned MP = 16;
  localparam int unsigned MemoryBytes = 64 * 1024;
  localparam int unsigned MemoryWords = MemoryBytes / 8;

  localparam logic [31:0] Input0 = 32'h0000;
  localparam logic [31:0] Weight = 32'h1000;
  localparam logic [31:0] Output0 = 32'h2000;
  localparam logic [31:0] Input1 = 32'h3000;
  localparam logic [31:0] Output1 = 32'h4000;

  logic clk = 1'b0;
  logic rst_n = 1'b0;

  logic [17:0] evt;
  logic busy;

  logic [MP-1:0] tcdm_req;
  logic [MP-1:0] tcdm_gnt;
  logic [MP-1:0][31:0] tcdm_add;
  logic [MP-1:0] tcdm_wen;
  logic [MP-1:0][MemDataWidth/8-1:0] tcdm_be;
  logic [MP-1:0][MemDataWidth-1:0] tcdm_data;
  logic [MP-1:0][MemDataWidth-1:0] tcdm_r_data;
  logic [MP-1:0] tcdm_r_valid;

  logic periph_req;
  logic periph_gnt;
  logic [31:0] periph_add;
  logic periph_wen;
  logic [3:0] periph_be;
  logic [31:0] periph_data;
  logic [1:0] periph_id;
  logic [31:0] periph_r_data;
  logic periph_r_valid;
  logic [1:0] periph_r_id;

  logic [63:0] memory [0:MemoryWords-1];

  always #1ns clk = ~clk;

  assign tcdm_gnt = tcdm_req;

  always_ff @(posedge clk) begin
    tcdm_r_valid <= '0;
    for (int lane = 0; lane < MP; lane++) begin
      if (tcdm_req[lane] && tcdm_gnt[lane]) begin
        if (tcdm_wen[lane]) begin
          tcdm_r_data[lane] <= memory[tcdm_add[lane][15:3]];
          tcdm_r_valid[lane] <= 1'b1;
        end else begin
          for (int byte_index = 0; byte_index < MemDataWidth/8; byte_index++) begin
            if (tcdm_be[lane][byte_index]) begin
              memory[tcdm_add[lane][15:3]][8*byte_index +: 8]
                <= tcdm_data[lane][8*byte_index +: 8];
            end
          end
        end
      end
    end
  end

  ita_hwpe_wrap #(
    .AccDataWidth(1024),
    .IdWidth(2),
    .MemDataWidth(MemDataWidth)
  ) dut (
    .clk_i(clk),
    .rst_ni(rst_n),
    .test_mode_i(1'b0),
    .evt_o(evt),
    .busy_o(busy),
    .tcdm_req_o(tcdm_req),
    .tcdm_gnt_i(tcdm_gnt),
    .tcdm_add_o(tcdm_add),
    .tcdm_wen_o(tcdm_wen),
    .tcdm_be_o(tcdm_be),
    .tcdm_data_o(tcdm_data),
    .tcdm_r_data_i(tcdm_r_data),
    .tcdm_r_valid_i(tcdm_r_valid),
    .periph_req_i(periph_req),
    .periph_gnt_o(periph_gnt),
    .periph_add_i(periph_add),
    .periph_wen_i(periph_wen),
    .periph_be_i(periph_be),
    .periph_data_i(periph_data),
    .periph_id_i(periph_id),
    .periph_r_data_o(periph_r_data),
    .periph_r_valid_o(periph_r_valid),
    .periph_r_id_o(periph_r_id)
  );

  task automatic periph_write(input logic [31:0] address,
                              input logic [31:0] data);
    @(negedge clk);
    periph_req = 1'b1;
    periph_add = address;
    periph_wen = 1'b0;
    periph_be = 4'hf;
    periph_data = data;
    do @(posedge clk); while (!periph_gnt);
    @(negedge clk);
    periph_req = 1'b0;
  endtask

  task automatic periph_read(input logic [31:0] address,
                             output logic [31:0] data);
    @(negedge clk);
    periph_req = 1'b1;
    periph_add = address;
    periph_wen = 1'b1;
    periph_be = 4'hf;
    do @(posedge clk); while (!periph_gnt);
    wait (periph_r_valid);
    data = periph_r_data;
    @(negedge clk);
    periph_req = 1'b0;
  endtask

  task automatic program_linear(input logic [31:0] input_address,
                                input logic [31:0] output_address,
                                input logic [7:0] right_shift);
    logic [31:0] acquired;
    periph_read(32'h04, acquired);
    if (acquired[31]) begin
      $fatal(1, "ITA context acquisition failed: 0x%08x", acquired);
    end

    periph_write(32'h20 + 4*0, input_address);
    periph_write(32'h20 + 4*1, Weight);
    periph_write(32'h20 + 4*2, Weight);
    periph_write(32'h20 + 4*3, 32'h0);
    periph_write(32'h20 + 4*4, output_address);
    periph_write(32'h20 + 4*5, 32'd64);
    periph_write(32'h20 + 4*6, 32'h00001111);
    periph_write(32'h20 + 4*7, 32'h01010101);
    periph_write(32'h20 + 4*8, 32'h01010101);
    periph_write(32'h20 + 4*9, {4{right_shift}});
    periph_write(32'h20 + 4*10, {4{right_shift}});
    periph_write(32'h20 + 4*11, 32'h0);
    periph_write(32'h20 + 4*12, 32'h0);
    periph_write(32'h20 + 4*13, 32'd2);
    periph_write(32'h20 + 4*14, 32'h00000005);
    periph_write(32'h20 + 4*15, 32'h0);
    periph_write(32'h20 + 4*16, 32'h0);
    periph_write(32'h00, 32'h0);
  endtask

  function automatic logic [7:0] get_byte(input logic [31:0] address);
    return memory[address[15:3]][8*address[2:0] +: 8];
  endfunction

  task automatic set_byte(input logic [31:0] address,
                          input logic [7:0] data);
    memory[address[15:3]][8*address[2:0] +: 8] = data;
  endtask

  initial begin
    logic [31:0] status;
    int errors;

    periph_req = 1'b0;
    periph_add = '0;
    periph_wen = 1'b1;
    periph_be = '0;
    periph_data = '0;
    periph_id = '0;
    tcdm_r_data = '0;
    tcdm_r_valid = '0;
    errors = 0;

    for (int word_index = 0; word_index < MemoryWords; word_index++) begin
      memory[word_index] = 64'h0;
    end
    for (int row = 0; row < 64; row++) begin
      for (int column = 0; column < 64; column++) begin
        set_byte(Input0 + row*64 + column, 1 + row % 3);
        set_byte(Input1 + row*64 + column, 1 + row % 5);
        set_byte(Weight + row*64 + column, 8'h01);
        set_byte(Output0 + row*64 + column, 8'hde);
        set_byte(Output1 + row*64 + column, 8'had);
      end
    end

    repeat (10) @(posedge clk);
    rst_n = 1'b1;
    repeat (3) @(posedge clk);

    periph_write(32'h14, 32'h0);
    program_linear(Input0, Output0, 8'd2);
    program_linear(Input1, Output1, 8'd3);

    status = 32'hffffffff;
    for (int poll = 0; poll < 100000 && status != 0; poll++) begin
      periph_read(32'h0c, status);
    end
    if (status != 0) begin
      $fatal(1, "ITA timed out with status 0x%08x", status);
    end

    repeat (10) @(posedge clk);
    for (int row = 0; row < 64; row++) begin
      logic [7:0] expected0 = 16 * (1 + row % 3);
      logic [7:0] expected1 = 8 * (1 + row % 5);
      for (int column = 0; column < 64; column++) begin
        logic [7:0] actual0 = get_byte(Output0 + row*64 + column);
        logic [7:0] actual1 = get_byte(Output1 + row*64 + column);
        if (actual0 != expected0) begin
          $display("operation 0 [%0d,%0d]: hardware=%0d software=%0d",
                   row, column, actual0, expected0);
          errors++;
        end
        if (actual1 != expected1) begin
          $display("operation 1 [%0d,%0d]: hardware=%0d software=%0d",
                   row, column, actual1, expected1);
          errors++;
        end
      end
    end

    if (errors != 0) begin
      $fatal(1, "ITA linear regression failed with %0d mismatches", errors);
    end
    $display("ITA linear PASS: two queued 64x64 operations match software");
    $finish;
  end

  initial begin
    #10ms;
    $fatal(1, "ITA smoke test timed out");
  end

endmodule
