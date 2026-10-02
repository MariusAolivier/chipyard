package chipyard.ne16

import chisel3._
import chisel3.util._

class NE16ScratchpadRequest extends Bundle {
  val address = UInt(32.W)
  val write = Bool()
  val data = UInt(64.W)
  val mask = UInt(8.W)
}

private class NE16BankRequest extends Bundle {
  val address = UInt(32.W)
  val write = Bool()
  val data = UInt(288.W)
  val mask = UInt(36.W)
  val wide = Bool()
}

class NE16Scratchpad(params: NE16Params) extends Module {
  val io = IO(new Bundle {
    val request = Vec(2, Flipped(Decoupled(new NE16ScratchpadRequest)))
    val response = Vec(2, Decoupled(UInt(64.W)))
    val tcdmRequest = Input(UInt(9.W))
    val tcdmAddresses = Input(UInt(288.W))
    val tcdmRead = Input(UInt(9.W))
    val tcdmData = Input(UInt(288.W))
    val tcdmMask = Input(UInt(36.W))
    val tcdmGrant = Output(UInt(9.W))
    val tcdmResponse = Output(UInt(288.W))
    val tcdmResponseValid = Output(UInt(9.W))
    val grants = Output(Vec(3, Bool()))
    val contention = Output(Bool())
  })

  private val bankCount = 9
  private val words = params.scratchpadBytes / 4
  private val wordBits = log2Ceil(words)
  private val bankBits = log2Ceil(bankCount)
  private val rows = (words + bankCount - 1) / bankCount
  private val memory = Seq.fill(bankCount) {
    SyncReadMem(rows, Vec(4, UInt(8.W)))
  }
  private def bankIndex(word: UInt): UInt = word % bankCount.U
  private def rowIndex(word: UInt): UInt = word / bankCount.U

  private val arbiter = Module(new RRArbiter(new NE16BankRequest, 3))
  val occupied = RegInit(VecInit(Seq.fill(2)(false.B)))
  val responseValid = RegInit(VecInit(Seq.fill(2)(false.B)))
  val responseData = Reg(Vec(2, UInt(64.W)))
  val responseBanks = Reg(Vec(2, Vec(2, UInt(bankBits.W))))
  val readIssued = Wire(Vec(2, Bool()))

  for (client <- 0 until 2) {
    val request = io.request(client)
    arbiter.io.in(client).valid := request.valid && !occupied(client)
    arbiter.io.in(client).bits.address := request.bits.address & "hfffffff8".U
    arbiter.io.in(client).bits.write := request.bits.write
    arbiter.io.in(client).bits.data := request.bits.data
    arbiter.io.in(client).bits.mask := request.bits.mask
    arbiter.io.in(client).bits.wide := false.B
    request.ready := arbiter.io.in(client).ready && !occupied(client)
    readIssued(client) := request.fire && !request.bits.write

    io.response(client).valid := responseValid(client)
    io.response(client).bits := responseData(client)
    when(request.fire) {
      occupied(client) := true.B
      responseValid(client) := request.bits.write
      responseData(client) := 0.U
    }
    when(io.response(client).fire) {
      occupied(client) := false.B
      responseValid(client) := false.B
    }
  }

  arbiter.io.in(2).valid := io.tcdmRequest.orR
  arbiter.io.in(2).bits.address := io.tcdmAddresses(31, 0)
  arbiter.io.in(2).bits.write := !io.tcdmRead(0)
  arbiter.io.in(2).bits.data := io.tcdmData
  arbiter.io.in(2).bits.mask := io.tcdmMask
  arbiter.io.in(2).bits.wide := true.B
  arbiter.io.out.ready := true.B
  io.tcdmGrant := Fill(9, arbiter.io.in(2).fire)
  io.grants := VecInit(arbiter.io.in.map(_.fire))
  io.contention := PopCount(VecInit(arbiter.io.in.map(_.valid))) > 1.U

  private val command = arbiter.io.out.bits
  val commandFire = arbiter.io.out.fire
  val firstWord = (command.address - params.scratchpadAddress.U)(wordBits + 1, 2)
  val laneWords = VecInit((0 until bankCount).map(lane => firstWord +& lane.U))
  val laneData = command.data.asTypeOf(Vec(bankCount, Vec(4, UInt(8.W))))
  val laneMask = command.mask.asTypeOf(Vec(bankCount, UInt(4.W)))
  val readWords = Wire(Vec(bankCount, UInt(32.W)))
  val tcdmBanks = Reg(Vec(bankCount, UInt(bankBits.W)))

  for (bank <- 0 until bankCount) {
    val readEnable = WireDefault(false.B)
    val writeEnable = WireDefault(false.B)
    val row = WireDefault(0.U(log2Ceil(rows).W))
    val data = WireDefault(0.U.asTypeOf(Vec(4, UInt(8.W))))
    val mask = WireDefault(0.U(4.W))
    for (lane <- 0 until bankCount) {
      when(commandFire && (command.wide || (lane < 2).B) &&
          bankIndex(laneWords(lane)) === bank.U) {
        readEnable := !command.write
        writeEnable := command.write
        row := rowIndex(laneWords(lane))
        data := laneData(lane)
        mask := laneMask(lane)
      }
    }
    val readData = memory(bank).read(row, readEnable)
    readWords(bank) := Cat(readData.reverse)
    when(writeEnable) {
      memory(bank).write(row, data, mask.asBools)
    }
  }

  for (client <- 0 until 2) {
    when(readIssued(client)) {
      for (lane <- 0 until 2) {
        responseBanks(client)(lane) := bankIndex(laneWords(lane))
      }
    }
    // Capture before a subsequent client's read can replace the SRAM outputs.
    when(RegNext(readIssued(client), false.B)) {
      responseData(client) := Cat(
        readWords(responseBanks(client)(1)),
        readWords(responseBanks(client)(0)))
      responseValid(client) := true.B
    }
    when(io.request(client).fire) {
      assert(io.request(client).bits.address >= params.scratchpadAddress.U)
      assert(io.request(client).bits.address <
        (params.scratchpadAddress + params.scratchpadBytes).U)
    }
  }

  val tcdmReadIssued = arbiter.io.in(2).fire && io.tcdmRead(0)
  for (lane <- 0 until bankCount) {
    when(tcdmReadIssued) {
      tcdmBanks(lane) := bankIndex(laneWords(lane))
    }
    when(io.tcdmRequest.orR) {
      val address = io.tcdmAddresses(32 * (lane + 1) - 1, 32 * lane)
      assert(address >= params.scratchpadAddress.U)
      assert(address < (params.scratchpadAddress + params.scratchpadBytes).U)
      assert(address(31, 2) === io.tcdmAddresses(31, 2) + lane.U)
    }
  }
  when(io.tcdmRequest.orR) {
    assert(io.tcdmRequest.andR)
    assert(io.tcdmRead === Fill(9, io.tcdmRead(0)))
  }
  io.tcdmResponse := Cat((0 until bankCount).reverse.map(lane =>
    readWords(tcdmBanks(lane))))
  io.tcdmResponseValid := Fill(9, RegNext(tcdmReadIssued, false.B))
}
