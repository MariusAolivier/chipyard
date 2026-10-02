package chipyard.ne16

import chisel3._
import chisel3.experimental.UnlocatableSourceInfo
import chisel3.simulator.EphemeralSimulator._
import freechips.rocketchip.diplomacy.{AddressSet, IdRange, TransferSizes}
import freechips.rocketchip.tilelink._
import org.chipsalliance.cde.config.Parameters
import org.scalatest.flatspec.AnyFlatSpec

private class NE16DMAHarness extends Module {
  private val params = NE16Params()
  private val edge = new TLEdgeOut(
    TLMasterPortParameters.v1(Seq(TLMasterParameters.v1(
      name = "test-dma", sourceId = IdRange(0, 1)))),
    TLSlavePortParameters.v1(Seq(TLSlaveParameters.v1(
      address = Seq(AddressSet(0x80000000L, 0xfff)),
      supportsGet = TransferSizes(1, 8),
      supportsPutFull = TransferSizes(1, 8))), beatBytes = 8, minLatency = 0),
    Parameters.empty, UnlocatableSourceInfo)
  val io = IO(new Bundle {
    val command = Flipped(chisel3.util.Decoupled(new NE16DMAConfig))
    val memory = new TLBundle(edge.bundle)
    val busy = Output(Bool())
    val done = Output(Bool())
    val error = Output(UInt(32.W))
    val bytesCopied = Output(UInt(32.W))
  })
  private val dma = Module(new NE16DMA(params, edge))
  private val storage = Module(new NE16Scratchpad(params))
  io.command <> dma.io.command
  io.memory <> dma.io.memory
  io.busy := dma.io.busy
  io.done := dma.io.done
  io.error := dma.io.error
  io.bytesCopied := dma.io.bytesCopied
  dma.io.acknowledge := false.B
  storage.io.request(1) <> dma.io.scratchpad
  dma.io.scratchpadResponse <> storage.io.response(1)
  storage.io.request(0).valid := false.B
  storage.io.request(0).bits := 0.U.asTypeOf(new NE16ScratchpadRequest)
  storage.io.response(0).ready := true.B
  storage.io.tcdmRequest := 0.U
  storage.io.tcdmAddresses := 0.U
  storage.io.tcdmRead := 0.U
  storage.io.tcdmData := 0.U
  storage.io.tcdmMask := 0.U
}

class NE16DMASpec extends AnyFlatSpec {
  behavior of "NE16 hardware DMA"

  it should "copy misaligned strided rows, preserve neighbors, and report bus errors" in {
    simulate(new NE16DMAHarness) { dut =>
      val external = scala.collection.mutable.Map.empty[BigInt, Int]
      val base = BigInt("80000000", 16)
      for (i <- 0 until 1024) { external(base + i) = (i * 7 + 3) & 255 }
      dut.io.command.valid.poke(false.B)
      dut.io.memory.a.ready.poke(false.B)
      dut.io.memory.d.valid.poke(false.B)
      dut.io.memory.d.bits.opcode.poke(TLMessages.AccessAckData)
      dut.io.memory.d.bits.param.poke(0.U)
      dut.io.memory.d.bits.size.poke(0.U)
      dut.io.memory.d.bits.source.poke(0.U)
      dut.io.memory.d.bits.sink.poke(0.U)
      dut.io.memory.d.bits.data.poke(0.U)
      dut.io.memory.d.bits.denied.poke(false.B)
      dut.io.memory.d.bits.corrupt.poke(false.B)
      dut.reset.poke(true.B)
      dut.clock.step(2)
      dut.reset.poke(false.B)

      def submit(source: BigInt, destination: BigInt, length: Int,
          rows: Int = 1, stride: Int = 24): Unit = {
        dut.io.command.bits.source.poke(source.U)
        dut.io.command.bits.destination.poke(destination.U)
        dut.io.command.bits.lineBytes.poke(length.U)
        dut.io.command.bits.rows.poke(rows.U)
        dut.io.command.bits.planes.poke(1.U)
        dut.io.command.bits.sourceRowStride.poke(stride.U)
        dut.io.command.bits.destinationRowStride.poke(stride.U)
        dut.io.command.bits.sourcePlaneStride.poke(0.U)
        dut.io.command.bits.destinationPlaneStride.poke(0.U)
        dut.io.command.valid.poke(true.B)
        dut.io.command.ready.expect(true.B)
        dut.clock.step()
        dut.io.command.valid.poke(false.B)
      }

      def finish(denied: Boolean = false, corrupt: Boolean = false,
          sameCycle: Boolean = false): Unit = {
        var response: Option[(BigInt, BigInt, Boolean)] = None
        var stalled: Option[(BigInt, BigInt, BigInt, BigInt)] = None
        var cycle = 0
        def driveResponse(value: Option[(BigInt, BigInt, Boolean)]): Unit = {
          dut.io.memory.d.valid.poke(value.nonEmpty.B)
          value.foreach { case (data, size, read) =>
            dut.io.memory.d.bits.data.poke(data.U)
            dut.io.memory.d.bits.size.poke(size.U)
            dut.io.memory.d.bits.opcode.poke(
              if (read) TLMessages.AccessAckData else TLMessages.AccessAck)
          }
        }
        while (dut.io.busy.peek().litToBoolean && cycle < 2000) {
          dut.io.memory.a.ready.poke((cycle % 4 == 3).B)
          driveResponse(response)
          dut.io.memory.d.bits.denied.poke(denied.B)
          dut.io.memory.d.bits.corrupt.poke(corrupt.B)
          val aValid = dut.io.memory.a.valid.peek().litToBoolean
          val aReady = dut.io.memory.a.ready.peek().litToBoolean
          var nextResponse: Option[(BigInt, BigInt, Boolean)] = None
          if (aValid) {
            val address = dut.io.memory.a.bits.address.peek().litValue
            val size = dut.io.memory.a.bits.size.peek().litValue
            val data = dut.io.memory.a.bits.data.peek().litValue
            val opcode = dut.io.memory.a.bits.opcode.peek().litValue
            val transaction = (address, size, data, opcode)
            stalled.foreach(previous => assert(transaction == previous,
              "TileLink request changed while stalled"))
            if (aReady) {
              stalled = None
              val read = opcode == TLMessages.Get.litValue
              val aligned = address & ~BigInt(7)
              val readData = (0 until 8).map(i =>
                BigInt(external(aligned + i)) << (8 * i)).reduce(_ | _)
              if (!read && !denied && !corrupt) {
                val mask = dut.io.memory.a.bits.mask.peek().litValue
                for (i <- 0 until 8 if mask.testBit(i)) {
                  external(aligned + i) = ((data >> (8 * i)) & 255).toInt
                }
              }
              nextResponse = Some((readData, size, read))
            } else { stalled = Some(transaction) }
          }
          if (sameCycle && nextResponse.nonEmpty) {
            assert(response.isEmpty, "DMA issued a request before its previous response")
            driveResponse(nextResponse)
          }
          val responseFired = dut.io.memory.d.valid.peek().litToBoolean &&
            dut.io.memory.d.ready.peek().litToBoolean
          dut.clock.step()
          if (responseFired) { response = None }
          if (nextResponse.nonEmpty && (!sameCycle || !responseFired)) {
            response = nextResponse
          }
          cycle += 1
        }
        assert(cycle < 2000, "DMA timed out")
        dut.io.memory.d.valid.poke(false.B)
        dut.io.done.expect(true.B)
      }

      val source = base + 3
      val destination = base + 257
      val before = external.toMap
      submit(source, BigInt("20000005", 16), 13, rows = 2)
      finish()
      dut.io.error.expect(0)
      dut.io.bytesCopied.expect(26)
      submit(BigInt("20000005", 16), destination, 13, rows = 2)
      finish()
      dut.io.error.expect(0)
      dut.io.bytesCopied.expect(26)
      for (i <- 0 until 1024) {
        val address = base + i
        val row = (address - destination).toInt / 24
        val offset = address - destination - row * 24
        val copied = address >= destination && row < 2 && offset >= 0 && offset < 13
        assert(external(address) == (if (copied) before(source + row * 24 + offset) else before(address)))
      }
      submit(base + 64, BigInt("20000200", 16), 16)
      finish(sameCycle = true)
      dut.io.error.expect(0)
      dut.io.bytesCopied.expect(16)
      submit(BigInt("20000200", 16), base + 512, 16)
      finish(sameCycle = true)
      dut.io.error.expect(0)
      dut.io.bytesCopied.expect(16)
      for (i <- 0 until 16) {
        assert(external(base + 512 + i) == before(base + 64 + i))
      }
      submit(source, BigInt("2000fffe", 16), 4)
      dut.io.done.expect(true.B)
      dut.io.error.expect(1)
      dut.io.bytesCopied.expect(0)
      submit(base + 0x2000, BigInt("20000005", 16), 8)
      finish()
      dut.io.error.expect(3)
      submit(source, BigInt("20000005", 16), 8)
      finish(denied = true)
      dut.io.error.expect(2)
      dut.io.bytesCopied.expect(0)
      submit(source, BigInt("20000005", 16), 8)
      finish(corrupt = true)
      dut.io.error.expect(2)
      dut.io.bytesCopied.expect(0)
    }
  }
}
