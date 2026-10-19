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
  private val beatBits = log2Ceil(burstBeats max 2)
  private val sources = params.dmaOutstanding
  require(edge.client.endSourceId == sources)

  def lastBeatOf(lgSize: UInt): UInt =
    Mux(lgSize >= 3.U, ((1.U((maxLgSize + 1).W) << lgSize) >> 3) - 1.U, 0.U)

  // A command walks chunks (generator), issues up to `sources` requests,
  // and moves data through the scratchpad port one beat at a time.
  val active = RegInit(false.B)
  val generated = Reg(Bool())
  val stopping = Reg(Bool())
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
  val done = RegInit(false.B)
  val error = RegInit(0.U(32.W))
  val bytesCopied = RegInit(0.U(32.W))

  class Context extends Bundle {
    val local = UInt(32.W)
    val lgSize = UInt(log2Ceil(maxLgSize + 1).W)
    val sourceOffset = UInt(3.W)
    val destinationOffset = UInt(3.W)
    val beat = UInt(beatBits.W)
  }
  val inFlight = RegInit(VecInit(Seq.fill(sources)(false.B)))
  val contexts = Reg(Vec(sources, new Context))
  val anyFree = !inFlight.asUInt.andR
  val freeSource = PriorityEncoder(inFlight.map(!_))

  // Store-side gather buffer for one chunk, and the Put being sent from it.
  val data = Reg(Vec(burstBeats, UInt(64.W)))
  val gatherBeat = RegInit(0.U(beatBits.W))
  val gathered = RegInit(false.B)
  val sendBeat = RegInit(0.U(beatBits.W))
  val sendSource = Reg(UInt(log2Ceil(sources max 2).W))
  val scratchWaiting = RegInit(false.B)

  def raise(code: UInt): Unit = {
    when(error === 0.U) { error := code }
    stopping := true.B
  }

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

  io.command.ready := !active
  io.busy := active
  io.done := done
  io.error := error
  io.bytesCopied := bytesCopied
  when(io.acknowledge && !active) {
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
      active := true.B
      generated := false.B
      stopping := false.B
      gatherBeat := 0.U
      gathered := false.B
      sendBeat := 0.U
    }.otherwise {
      done := true.B
      error := 1.U
    }
  }

  // Current chunk. Sub-beat chunks need both endpoints naturally aligned.
  // Bursts need only the external address aligned to the burst; scratch
  // beats are 8-aligned.
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
  val lastBeat = lastBeatOf(lgSize)

  def nextChunk(): Unit = {
    when(remaining > chunkBytes) {
      source := source + chunkBytes
      destination := destination + chunkBytes
      remaining := remaining - chunkBytes
    }.elsewhen(row + 1.U < config.rows) {
      sourceRow := sourceRow + config.sourceRowStride
      destinationRow := destinationRow + config.destinationRowStride
      source := sourceRow + config.sourceRowStride
      destination := destinationRow + config.destinationRowStride
      remaining := config.lineBytes
      row := row + 1.U
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
    }.otherwise {
      generated := true.B
    }
  }

  // TileLink A: Gets for loads, gathered Puts for stores. A started Put
  // burst always completes, even after an error.
  val sending = sendBeat =/= 0.U
  val issueLoad = active && load && !generated && !stopping && anyFree
  val issueStore = active && !load && gathered &&
    (sending || (!stopping && anyFree))
  val aSource = Mux(sending, sendSource, freeSource)
  val (_, get) = edge.Get(aSource, source, lgSize)
  val (_, put) = edge.Put(aSource, destination, lgSize,
    (data(sendBeat) << Cat(destination(2, 0), 0.U(3.W)))(63, 0))
  val externalLegal = edge.manager.containsSafe(externalAddress) &&
    Mux(load,
      edge.manager.supportsGetSafe(source, lgSize),
      edge.manager.supportsPutFullSafe(destination, lgSize))

  io.memory.a.valid := (issueLoad || issueStore) && externalLegal
  io.memory.a.bits := Mux(load, get, put)
  io.memory.b.ready := true.B
  io.memory.c.valid := false.B
  io.memory.c.bits := DontCare
  io.memory.e.valid := false.B
  io.memory.e.bits := DontCare

  when((issueLoad || (issueStore && !sending)) && !externalLegal) {
    raise(3.U)
  }
  when(io.memory.a.fire) {
    when(load) {
      inFlight(freeSource) := true.B
      val context = contexts(freeSource)
      context.local := destination
      context.lgSize := lgSize
      context.sourceOffset := source(2, 0)
      context.destinationOffset := destination(2, 0)
      context.beat := 0.U
      nextChunk()
    }.otherwise {
      when(!sending) {
        inFlight(freeSource) := true.B
        sendSource := freeSource
      }
      when(sendBeat =/= lastBeat) {
        sendBeat := sendBeat + 1.U
      }.otherwise {
        sendBeat := 0.U
        gathered := false.B
        bytesCopied := bytesCopied + chunkBytes
        nextChunk()
      }
    }
  }

  // TileLink D: load beats queue for the scratchpad writer; store acks free
  // their source immediately.
  class Beat extends Bundle {
    val data = UInt(64.W)
    val source = UInt(edge.bundle.sourceBits.W)
    val failed = Bool()
  }
  val beats = Module(new Queue(new Beat, 2))
  val memoryFailed = io.memory.d.bits.denied || io.memory.d.bits.corrupt
  beats.io.enq.valid := io.memory.d.valid && load
  beats.io.enq.bits.data := io.memory.d.bits.data
  beats.io.enq.bits.source := io.memory.d.bits.source
  beats.io.enq.bits.failed := memoryFailed
  io.memory.d.ready := Mux(load, beats.io.enq.ready, true.B)
  when(io.memory.d.fire && !load) {
    inFlight(io.memory.d.bits.source) := false.B
    when(memoryFailed) { raise(2.U) }
  }

  // Scratchpad port: load beats are written at their request's context;
  // store chunks are gathered beat by beat. One access in flight at a time.
  val head = beats.io.deq.bits
  val headContext = contexts(head.source)
  val headLast = headContext.beat === lastBeatOf(headContext.lgSize)
  val writeValid = active && load && beats.io.deq.valid && !head.failed &&
    !scratchWaiting
  val gatherValid = active && !load && !generated && !stopping &&
    !gathered && !scratchWaiting
  val sourceShift = Cat(Mux(load, headContext.sourceOffset, source(2, 0)), 0.U(3.W))
  val destinationShift = Cat(headContext.destinationOffset, 0.U(3.W))
  val smallBytes = 1.U(4.W) << headContext.lgSize(1, 0)
  val writeMask = Mux(headContext.lgSize >= 3.U, "hff".U(8.W),
    (((1.U(9.W) << smallBytes) - 1.U) << headContext.destinationOffset)(7, 0))

  io.scratchpad.valid := writeValid || gatherValid
  io.scratchpad.bits.address := Mux(load,
    headContext.local + (headContext.beat << 3),
    source + (gatherBeat << 3))
  io.scratchpad.bits.write := load
  io.scratchpad.bits.data :=
    ((head.data >> sourceShift) << destinationShift)(63, 0)
  io.scratchpad.bits.mask := writeMask
  io.scratchpadResponse.ready := scratchWaiting

  beats.io.deq.ready := load && (head.failed ||
    (io.scratchpad.ready && !scratchWaiting))
  when(beats.io.deq.fire) {
    when(headLast) {
      headContext.beat := 0.U
      inFlight(head.source) := false.B
      when(!head.failed) {
        bytesCopied := bytesCopied + (1.U << headContext.lgSize)
      }
    }.otherwise {
      headContext.beat := headContext.beat + 1.U
    }
    when(head.failed) { raise(2.U) }
  }
  when(io.scratchpad.fire) { scratchWaiting := true.B }
  when(io.scratchpadResponse.fire) {
    scratchWaiting := false.B
    when(!load) {
      data(gatherBeat) := io.scratchpadResponse.bits >> sourceShift
      when(gatherBeat === lastBeat) {
        gatherBeat := 0.U
        gathered := true.B
      }.otherwise {
        gatherBeat := gatherBeat + 1.U
      }
    }
  }

  // Finish once nothing is generated, queued, in flight or mid-burst.
  val drained = !inFlight.asUInt.orR && !beats.io.deq.valid &&
    !scratchWaiting && !sending
  when(active && (generated || stopping) && drained) {
    active := false.B
    done := true.B
  }
}
