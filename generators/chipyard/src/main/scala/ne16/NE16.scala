package chipyard.ne16

import chisel3._
import chisel3.experimental.IntParam
import chisel3.util._

import org.chipsalliance.cde.config.{Config, Field, Parameters}

import freechips.rocketchip.diplomacy._
import freechips.rocketchip.prci._
import freechips.rocketchip.resources.{MemoryDevice, SimpleDevice}
import freechips.rocketchip.subsystem.{BaseSubsystem, PBUS}
import freechips.rocketchip.tilelink._

case class NE16Params(
  controlAddress: BigInt = 0x10030000L,
  scratchpadAddress: BigInt = 0x20000000L,
  scratchpadBytes: Int = 64 * 1024
) {
  require(scratchpadBytes >= 4096)
  require((scratchpadBytes & (scratchpadBytes - 1)) == 0)
  require(scratchpadAddress + scratchpadBytes <= (BigInt(1) << 32))
}

case object NE16Key extends Field[Option[NE16Params]](None)

class NE16BlackBox extends BlackBox {
  override def desiredName = "NE16BlackBox"

  val io = IO(new Bundle {
    val clock = Input(Clock())
    val reset_n = Input(Bool())
    val evt_o = Output(UInt(2.W))
    val busy_o = Output(Bool())

    val tcdm_req_o = Output(UInt(9.W))
    val tcdm_gnt_i = Input(UInt(9.W))
    val tcdm_add_o = Output(UInt(288.W))
    val tcdm_wen_o = Output(UInt(9.W))
    val tcdm_be_o = Output(UInt(36.W))
    val tcdm_data_o = Output(UInt(288.W))
    val tcdm_r_data_i = Input(UInt(288.W))
    val tcdm_r_valid_i = Input(UInt(9.W))

    val periph_req_i = Input(Bool())
    val periph_gnt_o = Output(Bool())
    val periph_add_i = Input(UInt(32.W))
    val periph_wen_i = Input(Bool())
    val periph_be_i = Input(UInt(4.W))
    val periph_data_i = Input(UInt(32.W))
    val periph_r_data_o = Output(UInt(32.W))
    val periph_r_valid_o = Output(Bool())
  })
}

class NE16TL(params: NE16Params, beatBytes: Int)(implicit p: Parameters)
    extends ClockSinkDomain(ClockSinkParameters())(p) {
  require(beatBytes == 8, "NE16 integration currently requires a 64-bit peripheral bus")

  private val controlDevice =
    new SimpleDevice("ne16", Seq("pulp-platform,ne16"))
  private val scratchpadDevice = new MemoryDevice

  val controlNode = TLManagerNode(Seq(TLSlavePortParameters.v1(
    managers = Seq(TLSlaveParameters.v1(
      address = Seq(AddressSet(params.controlAddress, 0xfff)),
      resources = controlDevice.reg,
      regionType = RegionType.UNCACHED,
      executable = false,
      supportsGet = TransferSizes(1, beatBytes),
      supportsPutFull = TransferSizes(1, beatBytes),
      supportsPutPartial = TransferSizes(1, beatBytes),
      fifoId = Some(0))),
    beatBytes = beatBytes)))

  val scratchpadNode = TLManagerNode(Seq(TLSlavePortParameters.v1(
    managers = Seq(TLSlaveParameters.v1(
      address = Seq(AddressSet(params.scratchpadAddress, params.scratchpadBytes - 1)),
      resources = scratchpadDevice.reg,
      regionType = RegionType.UNCACHED,
      executable = false,
      supportsGet = TransferSizes(1, beatBytes),
      supportsPutFull = TransferSizes(1, beatBytes),
      supportsPutPartial = TransferSizes(1, beatBytes),
      fifoId = Some(0))),
    beatBytes = beatBytes)))

  override lazy val module = new NE16TLImpl

  class NE16TLImpl extends Impl {
    withClockAndReset(clock, reset) {
      val accelerator = Module(new NE16BlackBox)
      accelerator.io.clock := clock
      accelerator.io.reset_n := !reset.asBool

      val (control, controlEdge) = controlNode.in.head
      val idle :: request :: readResponse :: tileLinkResponse :: Nil = Enum(4)
      val controlState = RegInit(idle)
      val requestReg = Reg(chiselTypeOf(control.a.bits))
      val readDataReg = Reg(UInt(32.W))
      val requestIsRead = requestReg.opcode === TLMessages.Get
      val upperWord = requestReg.address(2)

      control.a.ready := controlState === idle
      when(control.a.fire) {
        requestReg := control.a.bits
        controlState := request
      }

      accelerator.io.periph_req_i := controlState === request
      accelerator.io.periph_add_i :=
        requestReg.address - params.controlAddress.U
      accelerator.io.periph_wen_i := requestIsRead
      accelerator.io.periph_be_i :=
        Mux(upperWord, requestReg.mask(7, 4), requestReg.mask(3, 0))
      accelerator.io.periph_data_i :=
        Mux(upperWord, requestReg.data(63, 32), requestReg.data(31, 0))

      when(controlState === request && accelerator.io.periph_gnt_o) {
        when(requestIsRead) {
          when(accelerator.io.periph_r_valid_o) {
            readDataReg := accelerator.io.periph_r_data_o
            controlState := tileLinkResponse
          }.otherwise {
            controlState := readResponse
          }
        }.otherwise {
          controlState := tileLinkResponse
        }
      }

      when(controlState === readResponse && accelerator.io.periph_r_valid_o) {
        readDataReg := accelerator.io.periph_r_data_o
        controlState := tileLinkResponse
      }

      control.d.valid := controlState === tileLinkResponse
      control.d.bits := controlEdge.AccessAck(requestReg)
      control.d.bits.opcode :=
        Mux(requestIsRead, TLMessages.AccessAckData, TLMessages.AccessAck)
      control.d.bits.data :=
        Mux(upperWord, Cat(readDataReg, 0.U(32.W)), Cat(0.U(32.W), readDataReg))
      when(control.d.fire) {
        controlState := idle
      }

      control.b.valid := false.B
      control.c.ready := true.B
      control.e.ready := true.B

      val words = params.scratchpadBytes / 4
      val wordIndexBits = log2Ceil(words)
      val memory = Mem(words, Vec(4, UInt(8.W)))
      val (scratchpad, scratchpadEdge) = scratchpadNode.in.head
      val scratchpadOffset =
        scratchpad.a.bits.address - params.scratchpadAddress.U
      val beatWordIndex =
        Cat(scratchpadOffset(log2Ceil(params.scratchpadBytes) - 1, 3), 0.U(1.W))
      val scratchpadWriteData =
        scratchpad.a.bits.data.asTypeOf(Vec(beatBytes, UInt(8.W)))
      val scratchpadHasData = scratchpadEdge.hasData(scratchpad.a.bits)

      def readWord(index: UInt): UInt = Cat(memory(index).reverse)

      scratchpad.a.ready := scratchpad.d.ready && !accelerator.io.busy_o
      scratchpad.d.valid := scratchpad.a.valid && !accelerator.io.busy_o
      scratchpad.d.bits := scratchpadEdge.AccessAck(scratchpad.a.bits)
      scratchpad.d.bits.opcode :=
        Mux(scratchpadHasData, TLMessages.AccessAck, TLMessages.AccessAckData)
      scratchpad.d.bits.data :=
        Cat(readWord(beatWordIndex + 1.U), readWord(beatWordIndex))

      when(scratchpad.a.fire && scratchpadHasData) {
        memory.write(
          beatWordIndex,
          VecInit(scratchpadWriteData.take(4)),
          scratchpad.a.bits.mask(3, 0).asBools)
        memory.write(
          beatWordIndex + 1.U,
          VecInit(scratchpadWriteData.drop(4)),
          scratchpad.a.bits.mask(7, 4).asBools)
      }

      scratchpad.b.valid := false.B
      scratchpad.c.ready := true.B
      scratchpad.e.ready := true.B

      val tcdmRequest = accelerator.io.tcdm_req_o.orR
      val tcdmRead = accelerator.io.tcdm_wen_o(0)
      val tcdmReadData = Wire(Vec(9, UInt(32.W)))
      val tcdmWordIndices = Wire(Vec(9, UInt(wordIndexBits.W)))

      for (lane <- 0 until 9) {
        val address = accelerator.io.tcdm_add_o(32 * (lane + 1) - 1, 32 * lane)
        val offset = address - params.scratchpadAddress.U
        tcdmWordIndices(lane) :=
          offset(log2Ceil(params.scratchpadBytes) - 1, 2)
        tcdmReadData(lane) := readWord(tcdmWordIndices(lane))

        when(tcdmRequest) {
          assert(address >= params.scratchpadAddress.U)
          assert(address < (params.scratchpadAddress + params.scratchpadBytes).U)
        }

        when(tcdmRequest && !tcdmRead) {
          val data =
            accelerator.io.tcdm_data_o(32 * (lane + 1) - 1, 32 * lane)
          val bytes = data.asTypeOf(Vec(4, UInt(8.W)))
          val mask =
            accelerator.io.tcdm_be_o(4 * (lane + 1) - 1, 4 * lane)
          memory.write(tcdmWordIndices(lane), bytes, mask.asBools)
        }
      }

      when(tcdmRequest) {
        assert(accelerator.io.tcdm_req_o.andR)
        assert(accelerator.io.tcdm_wen_o === Fill(9, tcdmRead))
      }

      val tcdmReadDataReg = Reg(Vec(9, UInt(32.W)))
      when(tcdmRequest && tcdmRead) {
        tcdmReadDataReg := tcdmReadData
      }
      val tcdmReadValid = RegNext(tcdmRequest && tcdmRead, false.B)

      accelerator.io.tcdm_gnt_i := Fill(9, true.B)
      accelerator.io.tcdm_r_data_i := Cat(tcdmReadDataReg.reverse)
      accelerator.io.tcdm_r_valid_i := Fill(9, tcdmReadValid)
    }
  }
}

trait CanHavePeripheryNE16 { this: BaseSubsystem =>
  private val pbus = locateTLBusWrapper(PBUS)

  val ne16 = p(NE16Key).map { params =>
    val accelerator = LazyModule(new NE16TL(params, pbus.beatBytes)(p))
    accelerator.clockNode := pbus.fixedClockNode

    pbus.coupleTo("ne16-control") {
      TLInwardClockCrossingHelper(
        "ne16_control_crossing",
        accelerator,
        accelerator.controlNode)(SynchronousCrossing()) :=
        TLFragmenter(pbus.beatBytes, pbus.blockBytes) := _
    }

    pbus.coupleTo("ne16-scratchpad") {
      TLInwardClockCrossingHelper(
        "ne16_scratchpad_crossing",
        accelerator,
        accelerator.scratchpadNode)(SynchronousCrossing()) :=
        TLFragmenter(pbus.beatBytes, pbus.blockBytes) := _
    }

    accelerator
  }
}

class WithNE16 extends Config((site, here, up) => {
  case NE16Key => Some(NE16Params())
})
