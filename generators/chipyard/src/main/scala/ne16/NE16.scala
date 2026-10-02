package chipyard.ne16

import chisel3._
import chisel3.util._

import org.chipsalliance.cde.config.{Config, Field, Parameters}

import freechips.rocketchip.diplomacy._
import freechips.rocketchip.prci._
import freechips.rocketchip.regmapper.{RegField, RegReadFn, RegWriteFn}
import freechips.rocketchip.resources.{MemoryDevice, SimpleDevice}
import freechips.rocketchip.subsystem.{BaseSubsystem, FBUS, PBUS}
import freechips.rocketchip.tilelink._

case class NE16Params(
  controlAddress: BigInt = 0x10030000L,
  scratchpadAddress: BigInt = 0x20000000L,
  scratchpadBytes: Int = 64 * 1024,
  dmaAddress: BigInt = 0x10031000L
) {
  require(scratchpadBytes >= 4096)
  require((scratchpadBytes & (scratchpadBytes - 1)) == 0)
  require(scratchpadAddress + scratchpadBytes <= (BigInt(1) << 32))
  require((scratchpadAddress & (scratchpadBytes - 1)) == 0)
  require((controlAddress & 0xfff) == 0 && (dmaAddress & 0xfff) == 0)
  require(controlAddress != dmaAddress)
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

  private val controlDevice = new SimpleDevice("ne16", Seq("pulp-platform,ne16"))
  private val scratchpadDevice = new MemoryDevice
  private val dmaDevice = new SimpleDevice("ne16-dma", Seq("chipyard,ne16-dma"))

  val dmaControlNode = TLRegisterNode(
    address = Seq(AddressSet(params.dmaAddress, 0xfff)),
    device = dmaDevice,
    beatBytes = beatBytes,
    concurrency = 1)
  val dmaMemoryNode = TLClientNode(Seq(TLMasterPortParameters.v1(
    Seq(TLMasterParameters.v1(name = "ne16-dma", sourceId = IdRange(0, 1))))))

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
      accelerator.io.periph_add_i := requestReg.address - params.controlAddress.U
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
      when(control.d.fire) { controlState := idle }
      control.b.valid := false.B
      control.c.ready := true.B
      control.e.ready := true.B

      val storage = Module(new NE16Scratchpad(params))
      val (scratchpad, scratchpadEdge) = scratchpadNode.in.head
      val scratchpadRequest = Reg(chiselTypeOf(scratchpad.a.bits))
      storage.io.request(0).valid := scratchpad.a.valid
      storage.io.request(0).bits.address := scratchpad.a.bits.address
      storage.io.request(0).bits.write := scratchpadEdge.hasData(scratchpad.a.bits)
      storage.io.request(0).bits.data := scratchpad.a.bits.data
      storage.io.request(0).bits.mask := scratchpad.a.bits.mask
      scratchpad.a.ready := storage.io.request(0).ready
      when(scratchpad.a.fire) { scratchpadRequest := scratchpad.a.bits }
      val scratchpadRead = scratchpadRequest.opcode === TLMessages.Get
      scratchpad.d.valid := storage.io.response(0).valid
      scratchpad.d.bits := scratchpadEdge.AccessAck(scratchpadRequest)
      scratchpad.d.bits.opcode :=
        Mux(scratchpadRead, TLMessages.AccessAckData, TLMessages.AccessAck)
      scratchpad.d.bits.data := storage.io.response(0).bits
      storage.io.response(0).ready := scratchpad.d.ready
      scratchpad.b.valid := false.B
      scratchpad.c.ready := true.B
      scratchpad.e.ready := true.B

      storage.io.tcdmRequest := accelerator.io.tcdm_req_o
      storage.io.tcdmAddresses := accelerator.io.tcdm_add_o
      storage.io.tcdmRead := accelerator.io.tcdm_wen_o
      storage.io.tcdmData := accelerator.io.tcdm_data_o
      storage.io.tcdmMask := accelerator.io.tcdm_be_o
      accelerator.io.tcdm_gnt_i := storage.io.tcdmGrant
      accelerator.io.tcdm_r_data_i := storage.io.tcdmResponse
      accelerator.io.tcdm_r_valid_i := storage.io.tcdmResponseValid

      val (dmaMemory, dmaEdge) = dmaMemoryNode.out.head
      val dma = Module(new NE16DMA(params, dmaEdge))
      dmaMemory <> dma.io.memory
      storage.io.request(1) <> dma.io.scratchpad
      dma.io.scratchpadResponse <> storage.io.response(1)
      val dmaConfig = RegInit(0.U.asTypeOf(new NE16DMAConfig))
      dma.io.command.bits := dmaConfig
      dma.io.command.valid := false.B
      dma.io.acknowledge := false.B

      def configField(value: UInt): RegField = RegField(32,
        RegReadFn(value), RegWriteFn((valid: Bool, data: UInt) => {
          when(valid && !dma.io.busy) { value := data }
          !dma.io.busy
        }))
      def commandWrite(valid: Bool, bits: UInt): Bool = {
        dma.io.command.valid := valid && bits(0)
        dma.io.acknowledge := valid && bits(1) && !dma.io.busy
        dma.io.command.ready
      }
      val status = Cat(0.U(29.W), dma.io.error =/= 0.U, dma.io.done, dma.io.busy)
      val grants = RegInit(VecInit(Seq.fill(3)(0.U(32.W))))
      val contention = RegInit(0.U(32.W))
      val dmaOverlap = RegInit(0.U(32.W))
      for (client <- 0 until 3) {
        when(storage.io.grants(client)) { grants(client) := grants(client) + 1.U }
      }
      when(storage.io.contention) { contention := contention + 1.U }
      when(storage.io.grants(1) && accelerator.io.busy_o) {
        dmaOverlap := dmaOverlap + 1.U
      }
      dmaControlNode.regmap(
        0x00 -> Seq(RegField.w(32, commandWrite(_, _))),
        0x04 -> Seq(RegField.r(32, status)),
        0x08 -> Seq(configField(dmaConfig.source)),
        0x0c -> Seq(configField(dmaConfig.destination)),
        0x10 -> Seq(configField(dmaConfig.lineBytes)),
        0x14 -> Seq(configField(dmaConfig.rows)),
        0x18 -> Seq(configField(dmaConfig.planes)),
        0x1c -> Seq(configField(dmaConfig.sourceRowStride)),
        0x20 -> Seq(configField(dmaConfig.destinationRowStride)),
        0x24 -> Seq(configField(dmaConfig.sourcePlaneStride)),
        0x28 -> Seq(configField(dmaConfig.destinationPlaneStride)),
        0x2c -> Seq(RegField.r(32, dma.io.error)),
        0x30 -> Seq(RegField.r(32, dma.io.bytesCopied)),
        0x38 -> Seq(RegField.r(32, grants(0))),
        0x3c -> Seq(RegField.r(32, grants(1))),
        0x40 -> Seq(RegField.r(32, grants(2))),
        0x44 -> Seq(RegField.r(32, contention)),
        0x48 -> Seq(RegField.r(32, dmaOverlap)))
    }
  }
}

trait CanHavePeripheryNE16 { this: BaseSubsystem =>
  private val pbus = locateTLBusWrapper(PBUS)
  private val fbus = locateTLBusWrapper(FBUS)

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
    pbus.coupleTo("ne16-dma-control") {
      TLInwardClockCrossingHelper(
        "ne16_dma_control_crossing",
        accelerator,
        accelerator.dmaControlNode)(SynchronousCrossing()) :=
        TLFragmenter(pbus.beatBytes, pbus.blockBytes) := _
    }
    fbus.coupleFrom("ne16-dma-memory") {
      _ := TLWidthWidget(8) :=
        TLOutwardClockCrossingHelper(
          "ne16_dma_memory_crossing",
          accelerator,
          accelerator.dmaMemoryNode)(SynchronousCrossing())
    }

    accelerator
  }
}

class WithNE16 extends Config((site, here, up) => {
  case NE16Key => Some(NE16Params())
})
