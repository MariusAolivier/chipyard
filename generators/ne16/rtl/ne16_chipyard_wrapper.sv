module NE16BlackBox (
  input  logic         clock,
  input  logic         reset_n,
  output logic [1:0]   evt_o,
  output logic         busy_o,
  output logic [8:0]   tcdm_req_o,
  input  logic [8:0]   tcdm_gnt_i,
  output logic [287:0] tcdm_add_o,
  output logic [8:0]   tcdm_wen_o,
  output logic [35:0]  tcdm_be_o,
  output logic [287:0] tcdm_data_o,
  input  logic [287:0] tcdm_r_data_i,
  input  logic [8:0]   tcdm_r_valid_i,
  input  logic         periph_req_i,
  output logic         periph_gnt_o,
  input  logic [31:0]  periph_add_i,
  input  logic         periph_wen_i,
  input  logic [3:0]   periph_be_i,
  input  logic [31:0]  periph_data_i,
  output logic [31:0]  periph_r_data_o,
  output logic         periph_r_valid_o
);

  logic [15:0] periph_r_id_unused;

  ne16_top_wrap #(
    .ID(16),
    .N_CORES(1)
  ) i_ne16 (
    .clk_i(clock),
    .rst_ni(reset_n),
    .test_mode_i(1'b0),
    .evt_o(evt_o),
    .busy_o(busy_o),
    .tcdm_req(tcdm_req_o),
    .tcdm_gnt(tcdm_gnt_i),
    .tcdm_add(tcdm_add_o),
    .tcdm_wen(tcdm_wen_o),
    .tcdm_be(tcdm_be_o),
    .tcdm_data(tcdm_data_o),
    .tcdm_r_data(tcdm_r_data_i),
    .tcdm_r_valid(tcdm_r_valid_i),
    .periph_req(periph_req_i),
    .periph_gnt(periph_gnt_o),
    .periph_add(periph_add_i),
    .periph_wen(periph_wen_i),
    .periph_be(periph_be_i),
    .periph_data(periph_data_i),
    .periph_id(16'b0),
    .periph_r_data(periph_r_data_o),
    .periph_r_valid(periph_r_valid_o),
    .periph_r_id(periph_r_id_unused)
  );

endmodule
