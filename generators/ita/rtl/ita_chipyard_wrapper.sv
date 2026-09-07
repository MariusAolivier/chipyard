module ITABlackBox (
  input  logic          clock,
  input  logic          reset_n,
  output logic [17:0]   evt_o,
  output logic          busy_o,
  output logic [15:0]   tcdm_req_o,
  input  logic [15:0]   tcdm_gnt_i,
  output logic [511:0]  tcdm_add_o,
  output logic [15:0]   tcdm_wen_o,
  output logic [127:0]  tcdm_be_o,
  output logic [1023:0] tcdm_data_o,
  input  logic [1023:0] tcdm_r_data_i,
  input  logic [15:0]   tcdm_r_valid_i,
  input  logic          periph_req_i,
  output logic          periph_gnt_o,
  input  logic [31:0]   periph_add_i,
  input  logic          periph_wen_i,
  input  logic [3:0]    periph_be_i,
  input  logic [31:0]   periph_data_i,
  output logic [31:0]   periph_r_data_o,
  output logic          periph_r_valid_o
);

  logic [1:0] periph_r_id_unused;

  ita_hwpe_wrap #(
    .AccDataWidth(1024),
    .IdWidth(2),
    .MemDataWidth(64)
  ) i_ita (
    .clk_i(clock),
    .rst_ni(reset_n),
    .test_mode_i(1'b0),
    .evt_o(evt_o),
    .busy_o(busy_o),
    .tcdm_req_o(tcdm_req_o),
    .tcdm_gnt_i(tcdm_gnt_i),
    .tcdm_add_o(tcdm_add_o),
    .tcdm_wen_o(tcdm_wen_o),
    .tcdm_be_o(tcdm_be_o),
    .tcdm_data_o(tcdm_data_o),
    .tcdm_r_data_i(tcdm_r_data_i),
    .tcdm_r_valid_i(tcdm_r_valid_i),
    .periph_req_i(periph_req_i),
    .periph_gnt_o(periph_gnt_o),
    .periph_add_i(periph_add_i),
    .periph_wen_i(periph_wen_i),
    .periph_be_i(periph_be_i),
    .periph_data_i(periph_data_i),
    .periph_id_i(2'b0),
    .periph_r_data_o(periph_r_data_o),
    .periph_r_valid_o(periph_r_valid_o),
    .periph_r_id_o(periph_r_id_unused)
  );

endmodule
