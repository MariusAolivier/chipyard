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

  // Largest burst every DRAM manager accepts for both Get and PutFull.
  private val burstBytes = {
    val dram = edge.manager.managers.filter(
      _.address.exists(_.contains(BigInt(0x80000000L))))
    (Seq(params.dmaBurstBytes) ++
      dram.map(m => m.supportsGet.max min m.supportsPutFull.max)).min
  }
  require(burstBytes >= 8 && isPow2(burstBytes),
    s"NE16 DMA needs at least 8-byte DRAM transfers, got $burstBytes")
  private val maxLgSize = log2Ceil(burstBytes)
  private val burstBeats = burstBytes / 8

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
  // One burst of data: loads use entry 0 per beat, stores gather all beats.
  val data = Reg(Vec(burstBeats, UInt(64.W)))
  val beat = RegInit(0.U(log2Ceil(burstBeats + 1).W))
  val acknowledged = RegInit(false.B)
  val denied = RegInit(false.B)
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
      beat := 0.U
      state := Mux(destinationLocal, externalRead, localRead)
    }.otherwise {
      done := true.B
      error := 1.U
    }
  }

  // Sub-beat chunks need both endpoints naturally aligned. Bursts need only
  // the external address aligned to the burst; scratch beats are 8-aligned.
  val externalAddress = Mux(load, source, destination)
  val localAddress = Mux(load, destination, source)
  val lgSize = WireDefault(0.U(log2Ceil(maxLgSize + 1).W))
  for (size <- 1 to maxLgSize) {
    val aligned = if (size <= 3) {
      source(size - 1, 0) === 0.U && destination(size - 1, 0) === 0.U
    } else {
      externalAddress(size - 1, 0) === 0.U && localAddress(2, 0) === 0.U
    }
    when(remaining >= (1 << size).U && aligned) { lgSize := size.U }
  }
  val chunkBytes = 1.U((maxLgSize + 1).W) << lgSize
  val burst = lgSize >= 3.U
  val lastBeat = Mux(burst, (chunkBytes >> 3) - 1.U, 0.U)
  val sourceShift = Cat(source(2, 0), 0.U(3.W))
  val destinationShift = Cat(destination(2, 0), 0.U(3.W))
  val smallBytes = 1.U(4.W) << lgSize(1, 0)
  val destinationMask = Mux(burst, "hff".U(8.W),
    (((1.U(9.W) << smallBytes) - 1.U) << destination(2, 0))(7, 0))
  val (_, get) = edge.Get(0.U, source, lgSize)
  val (_, put) = edge.Put(0.U, destination, lgSize,
    (data(beat) << destinationShift)(63, 0))
  val externalLegal = edge.manager.containsSafe(externalAddress) &&
    Mux(load,
      edge.manager.supportsGetSafe(source, lgSize),
      edge.manager.supportsPutFullSafe(destination, lgSize))

  io.memory.a.valid := (state === externalRead || state === externalWrite) && externalLegal
  io.memory.a.bits := Mux(state === externalRead, get, put)
  io.memory.d.ready := state === externalReadResponse ||
    state === externalWrite || state === externalWriteResponse
  io.memory.b.ready := true.B
  io.memory.c.valid := false.B
  io.memory.c.bits := DontCare
  io.memory.e.valid := false.B
  io.memory.e.bits := DontCare

  io.scratchpad.valid := state === localRead || state === localWrite
  io.scratchpad.bits.address := localAddress + (beat << 3)
  io.scratchpad.bits.write := load
  io.scratchpad.bits.data := (data(0) << destinationShift)(63, 0)
  io.scratchpad.bits.mask := destinationMask
  io.scratchpadResponse.ready := state === localReadResponse || state === localWriteResponse

  val memoryFailed = io.memory.d.bits.denied || io.memory.d.bits.corrupt
  def fail(code: UInt): Unit = {
    state := idle
    done := true.B
    error := code
  }

  when((state === externalRead || state === externalWrite) && !externalLegal) {
    fail(3.U)
  }

  // Load: one Get, then each returned beat is written to the scratchpad.
  when(io.memory.a.fire && load) {
    state := externalReadResponse
  }
  when(io.memory.d.fire && load) {
    when(memoryFailed || denied) {
      // Consume the remaining beats so the bus is left idle.
      denied := beat =/= lastBeat
      beat := beat + 1.U
      when(beat === lastBeat) { fail(2.U) }
    }.otherwise {
      data(0) := io.memory.d.bits.data >> sourceShift
      state := localWrite
    }
  }
  when(io.scratchpad.fire) {
    state := Mux(load, localWriteResponse, localReadResponse)
  }
  when(io.scratchpadResponse.fire) {
    val more = beat =/= lastBeat
    when(load) {
      state := Mux(more, externalReadResponse, advance)
    }.otherwise {
      data(beat) := io.scratchpadResponse.bits >> sourceShift
      state := Mux(more, localRead, externalWrite)
    }
    beat := Mux(more, beat + 1.U, 0.U)
  }

  // Store: gather all beats, send them as one Put, then await its ack. The
  // ack may arrive before the last beat; the burst is always completed.
  val ackNow = io.memory.d.fire && !load
  val ackFailed = denied || (ackNow && memoryFailed)
  def finishStore(): Unit = {
    when(ackFailed) { fail(2.U) }.otherwise { state := advance }
  }
  when(ackNow) {
    acknowledged := true.B
    when(memoryFailed) { denied := true.B }
  }
  when(io.memory.a.fire && !load) {
    when(beat =/= lastBeat) {
      beat := beat + 1.U
    }.otherwise {
      beat := 0.U
      when(acknowledged || ackNow) { finishStore() }
        .otherwise { state := externalWriteResponse }
    }
  }
  when(state === externalWriteResponse && ackNow) { finishStore() }

  when(state === idle || state === advance) {
    acknowledged := false.B
    denied := false.B
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
