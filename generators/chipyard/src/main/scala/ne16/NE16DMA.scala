package chipyard.ne16

import chisel3._
import chisel3.util._
import freechips.rocketchip.tilelink._

class NE16DMAConfig extends Bundle {
  val source = UInt(32.W)
  val destination = UInt(32.W)
  val lineBytes = UInt(32.W)
  val rows = UInt(32.W)
  val planes = UInt(32.W)
  val sourceRowStride = UInt(32.W)
  val destinationRowStride = UInt(32.W)
  val sourcePlaneStride = UInt(32.W)
  val destinationPlaneStride = UInt(32.W)
}

class NE16DMA(params: NE16Params, edge: TLEdgeOut) extends Module {
  require(edge.bundle.dataBits == 64)
  require(edge.bundle.addressBits >= 32)
  val io = IO(new Bundle {
    val command = Flipped(Decoupled(new NE16DMAConfig))
    val acknowledge = Input(Bool())
    val busy = Output(Bool())
    val done = Output(Bool())
    val error = Output(UInt(32.W))
    val bytesCopied = Output(UInt(32.W))
    val memory = new TLBundle(edge.bundle)
    val scratchpad = Decoupled(new NE16ScratchpadRequest)
    val scratchpadResponse = Flipped(Decoupled(UInt(64.W)))
  })

  val (idle :: externalRead :: externalReadResponse :: localWrite ::
    localWriteResponse :: localRead :: localReadResponse :: externalWrite ::
    externalWriteResponse :: advance :: Nil) = Enum(10)
  val state = RegInit(idle)
  val config = Reg(new NE16DMAConfig)
  val source = Reg(UInt(32.W))
  val destination = Reg(UInt(32.W))
  val sourceRow = Reg(UInt(32.W))
  val destinationRow = Reg(UInt(32.W))
  val sourcePlane = Reg(UInt(32.W))
  val destinationPlane = Reg(UInt(32.W))
  val remaining = Reg(UInt(32.W))
  val row = Reg(UInt(32.W))
  val plane = Reg(UInt(32.W))
  val load = Reg(Bool())
  val data = Reg(UInt(64.W))
  val done = RegInit(false.B)
  val error = RegInit(0.U(32.W))
  val bytesCopied = RegInit(0.U(32.W))

  def inScratch(address: UInt): Bool =
    address >= params.scratchpadAddress.U &&
      address < (params.scratchpadAddress + params.scratchpadBytes).U

  def span(rows: UInt, planes: UInt, line: UInt, rowStride: UInt,
      planeStride: UInt): UInt =
    ((planes - 1.U) * planeStride) +&
      (((rows - 1.U) * rowStride) +& line)

  def validEndpoint(address: UInt, size: UInt, local: Bool): Bool = {
    val end = address +& size
    Mux(local,
      end <= (params.scratchpadAddress + params.scratchpadBytes).U,
      address >= "h80000000".U && end <= (BigInt(1) << 32).U)
  }

  val candidate = io.command.bits
  val sourceLocal = inScratch(candidate.source)
  val destinationLocal = inScratch(candidate.destination)
  val sourceSpan = span(candidate.rows, candidate.planes, candidate.lineBytes,
    candidate.sourceRowStride, candidate.sourcePlaneStride)
  val destinationSpan = span(candidate.rows, candidate.planes, candidate.lineBytes,
    candidate.destinationRowStride, candidate.destinationPlaneStride)
  val sourceRowSpan = ((candidate.rows - 1.U) * candidate.sourceRowStride) +& candidate.lineBytes
  val destinationRowSpan = ((candidate.rows - 1.U) * candidate.destinationRowStride) +& candidate.lineBytes
  val totalBytes = candidate.lineBytes * candidate.rows * candidate.planes
  val validConfig = candidate.lineBytes =/= 0.U &&
    candidate.rows =/= 0.U && candidate.planes =/= 0.U &&
    totalBytes <= "hffffffff".U &&
    (sourceLocal ^ destinationLocal) &&
    validEndpoint(candidate.source, sourceSpan, sourceLocal) &&
    validEndpoint(candidate.destination, destinationSpan, destinationLocal) &&
    (candidate.rows === 1.U ||
      (candidate.sourceRowStride >= candidate.lineBytes &&
        candidate.destinationRowStride >= candidate.lineBytes)) &&
    (candidate.planes === 1.U ||
      (candidate.sourcePlaneStride >= sourceRowSpan &&
        candidate.destinationPlaneStride >= destinationRowSpan))

  io.command.ready := state === idle
  io.busy := state =/= idle
  io.done := done
  io.error := error
  io.bytesCopied := bytesCopied
  when(io.acknowledge && state === idle) {
    done := false.B
    error := 0.U
  }
  when(io.command.fire) {
    done := false.B
    error := 0.U
    bytesCopied := 0.U
    when(validConfig) {
      config := candidate
      source := candidate.source
      destination := candidate.destination
      sourceRow := candidate.source
      destinationRow := candidate.destination
      sourcePlane := candidate.source
      destinationPlane := candidate.destination
      remaining := candidate.lineBytes
      row := 0.U
      plane := 0.U
      load := destinationLocal
      state := Mux(destinationLocal, externalRead, localRead)
    }.otherwise {
      done := true.B
      error := 1.U
    }
  }

  // Use the largest naturally aligned transfer that fits both endpoints.
  val lgSize = WireDefault(0.U(2.W))
  for (size <- 1 to 3) {
    when(remaining >= (1 << size).U &&
        source(size - 1, 0) === 0.U &&
        destination(size - 1, 0) === 0.U) {
      lgSize := size.U
    }
  }
  val chunkBytes = 1.U(4.W) << lgSize
  val sourceShift = Cat(source(2, 0), 0.U(3.W))
  val destinationShift = Cat(destination(2, 0), 0.U(3.W))
  val destinationMask = (((1.U(9.W) << chunkBytes) - 1.U) << destination(2, 0))(7, 0)
  val (_, get) = edge.Get(0.U, source, lgSize)
  val (_, put) = edge.Put(0.U, destination, lgSize,
    (data << destinationShift)(63, 0))
  val externalAddress = Mux(load, source, destination)
  val externalLegal = edge.manager.containsSafe(externalAddress) &&
    Mux(load,
      edge.manager.supportsGetSafe(source, lgSize),
      edge.manager.supportsPutFullSafe(destination, lgSize))

  io.memory.a.valid := (state === externalRead || state === externalWrite) && externalLegal
  io.memory.a.bits := Mux(state === externalRead, get, put)
  io.memory.d.ready := state === externalRead || state === externalWrite ||
    state === externalReadResponse || state === externalWriteResponse
  io.memory.b.ready := true.B
  io.memory.c.valid := false.B
  io.memory.c.bits := DontCare
  io.memory.e.valid := false.B
  io.memory.e.bits := DontCare

  io.scratchpad.valid := state === localRead || state === localWrite
  io.scratchpad.bits.address := Mux(load, destination, source)
  io.scratchpad.bits.write := load
  io.scratchpad.bits.data := (data << destinationShift)(63, 0)
  io.scratchpad.bits.mask := destinationMask
  io.scratchpadResponse.ready := state === localReadResponse || state === localWriteResponse

  when((state === externalRead || state === externalWrite) && !externalLegal) {
    state := idle
    done := true.B
    error := 3.U
  }
  when(io.memory.a.fire) {
    state := Mux(load, externalReadResponse, externalWriteResponse)
  }
  when(io.memory.d.fire) {
    when(io.memory.d.bits.denied || io.memory.d.bits.corrupt) {
      state := idle
      done := true.B
      error := 2.U
    }.otherwise {
      when(load) {
        data := io.memory.d.bits.data >> sourceShift
        state := localWrite
      }.otherwise {
        state := advance
      }
    }
  }
  when(io.scratchpad.fire) {
    state := Mux(load, localWriteResponse, localReadResponse)
  }
  when(io.scratchpadResponse.fire) {
    when(load) {
      state := advance
    }.otherwise {
      data := io.scratchpadResponse.bits >> sourceShift
      state := externalWrite
    }
  }

  when(state === advance) {
    bytesCopied := bytesCopied + chunkBytes
    when(remaining > chunkBytes) {
      source := source + chunkBytes
      destination := destination + chunkBytes
      remaining := remaining - chunkBytes
      state := Mux(load, externalRead, localRead)
    }.elsewhen(row + 1.U < config.rows) {
      sourceRow := sourceRow + config.sourceRowStride
      destinationRow := destinationRow + config.destinationRowStride
      source := sourceRow + config.sourceRowStride
      destination := destinationRow + config.destinationRowStride
      remaining := config.lineBytes
      row := row + 1.U
      state := Mux(load, externalRead, localRead)
    }.elsewhen(plane + 1.U < config.planes) {
      sourcePlane := sourcePlane + config.sourcePlaneStride
      destinationPlane := destinationPlane + config.destinationPlaneStride
      sourceRow := sourcePlane + config.sourcePlaneStride
      destinationRow := destinationPlane + config.destinationPlaneStride
      source := sourcePlane + config.sourcePlaneStride
      destination := destinationPlane + config.destinationPlaneStride
      remaining := config.lineBytes
      row := 0.U
      plane := plane + 1.U
      state := Mux(load, externalRead, localRead)
    }.otherwise {
      state := idle
      done := true.B
    }
  }
}
