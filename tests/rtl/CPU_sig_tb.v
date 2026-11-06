`timescale 1ns/1ps
// vvizard-owned signature TB for tri-way diff. Instantiates cs150 CoreHarness unchanged.
// Prints machine-parseable SIG lines; does not modify CPU RTL.
`ifndef MAIN_HEX
 `define MAIN_HEX "main.hex"
`endif
`ifndef NSTALL
 `define NSTALL 4
`endif
`ifndef MAX_CYCLES
 `define MAX_CYCLES 50000
`endif
`ifndef ISR_HEX
 `define ISR_HEX "isr_nop.hex"
`endif
module CPU_sig_tb;
  localparam RESULTS_W = 32'h400 >> 2;
  reg clk=0, rst=1;
  always #10 clk = ~clk;
  CoreHarness #(.NSTALL(`NSTALL), .MAIN_HEX(`MAIN_HEX), .ISR_HEX(`ISR_HEX)) h(
    .clk(clk), .rst(rst), .irq_tx(1'b0));

  integer cyc=0, i, n, errs=0;
  reg [31:0] act, exp;
  initial begin
    repeat(5) @(posedge clk); rst = 0;
    while (h.x5[0] !== 32'h600DC0DE && cyc < `MAX_CYCLES) begin
      @(posedge clk); cyc = cyc + 1;
    end
    repeat(4) @(posedge clk);
    if (h.x5[0] !== 32'h600DC0DE) begin
      errs = errs + 1;
      $display("TIMEOUT: no DONE_MAGIC in %0d cycles PC_DX=%h", `MAX_CYCLES, h.core.PC_DX);
      n = 0;
    end else n = h.x5[1];
    $display("SIG n=%0d", n);
    for (i=0; i<n; i=i+1) begin
      act = h.x5[RESULTS_W + 2*i];
      exp = h.x5[RESULTS_W + 2*i + 1];
      $display("SIG i=%0d act=%h exp=%h", i, act, exp);
      if (act !== exp) begin
        errs = errs + 1;
        $display("ERROR check #%0d: got %h want %h", i, act, exp);
      end
    end
    if (n == 0 && errs == 0) begin errs = errs + 1; $display("ERROR no CHECK results"); end
`ifdef FORCE_FAIL
    errs = errs + 1; $display("ERROR forced failure (FORCE_FAIL)");
`endif
    if (errs == 0) $display("PASS: CPU_sig_tb %0s (%0d checks, %0d cycles)", `MAIN_HEX, n, cyc);
    else $display("FAIL: CPU_sig_tb %0s (%0d errors)", `MAIN_HEX, errs);
    $finish;
  end
endmodule
