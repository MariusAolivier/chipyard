package chipyard.ne16

import chisel3._
import chisel3.simulator.EphemeralSimulator._
import org.scalatest.flatspec.AnyFlatSpec

class NE16ScratchpadSpec extends AnyFlatSpec {
  private val params = NE16Params()
  private val base = params.scratchpadAddress

  private def initialize(dut: NE16Scratchpad): Unit = {
    for (client <- 0 until 2) {
      dut.io.request(client).valid.poke(false.B)
      dut.io.request(client).bits.address.poke(base.U)
      dut.io.request(client).bits.write.poke(false.B)
      dut.io.request(client).bits.data.poke(0.U)
      dut.io.request(client).bits.mask.poke(255.U)
      dut.io.response(client).ready.poke(true.B)
    }
    dut.io.tcdmRequest.poke(0.U)
    dut.io.tcdmAddresses.poke(addresses(0).U)
    dut.io.tcdmRead.poke(511.U)
    dut.io.tcdmData.poke(0.U)
    dut.io.tcdmMask.poke(((BigInt(1) << 36) - 1).U)
    dut.reset.poke(true.B)
    dut.clock.step(2)
    dut.reset.poke(false.B)
  }

  private def addresses(first: Int): BigInt =
    (0 until 9).map(lane => (base + 4 * (first + lane)) << (32 * lane)).reduce(_ | _)

  private def waitFor(condition: => Boolean, dut: NE16Scratchpad): Unit = {
    var cycles = 0
    while (!condition && cycles < 20) {
      dut.clock.step()
      cycles += 1
    }
    assert(condition, "scratchpad handshake timed out")
  }

  private def write(dut: NE16Scratchpad, client: Int, offset: Int,
      data: BigInt, mask: Int = 255): Unit = {
    val request = dut.io.request(client)
    request.bits.address.poke((base + offset).U)
    request.bits.write.poke(true.B)
    request.bits.data.poke(data.U)
    request.bits.mask.poke(mask.U)
    request.valid.poke(true.B)
    waitFor(request.ready.peek().litToBoolean, dut)
    dut.clock.step()
    request.valid.poke(false.B)
    waitFor(dut.io.response(client).valid.peek().litToBoolean, dut)
    dut.clock.step()
  }

  behavior of "NE16 whole-request scratchpad arbitration"

  it should "rotate fairly across CPU, DMA, and all nine NE16 lanes" in {
    simulate(new NE16Scratchpad(params)) { dut =>
      initialize(dut)
      for (client <- 0 until 2) {
        dut.io.request(client).bits.write.poke(true.B)
        dut.io.request(client).valid.poke(true.B)
      }
      dut.io.tcdmRequest.poke(511.U)
      val counts = Array.fill(3)(0)
      val waits = Array.fill(3)(0)
      for (_ <- 0 until 90) {
        assert((0 until 3).count(i => dut.io.grants(i).peek().litToBoolean) == 1)
        val eligible = Seq(
          dut.io.request(0).ready.peek().litToBoolean || !dut.io.response(0).valid.peek().litToBoolean,
          dut.io.request(1).ready.peek().litToBoolean || !dut.io.response(1).valid.peek().litToBoolean,
          true)
        for (client <- 0 until 3) {
          if (dut.io.grants(client).peek().litToBoolean) {
            counts(client) += 1
            waits(client) = 0
          } else if (eligible(client)) {
            waits(client) += 1
            assert(waits(client) <= 2, s"client $client starved")
          }
        }
        val grant = dut.io.tcdmGrant.peek().litValue
        assert(grant == 0 || grant == 511)
        dut.clock.step()
      }
      assert(counts.forall(_ >= 29), s"unfair grants: ${counts.toSeq}")
    }
  }

  it should "return nine words next cycle for every bank rotation and row crossing" in {
    simulate(new NE16Scratchpad(params)) { dut =>
      initialize(dut)
      for (word <- 0 until 40 by 2) {
        write(dut, 0, word * 4, BigInt(word + 1) | (BigInt(word + 2) << 32))
      }
      for (first <- 0 until 18) {
        dut.io.tcdmAddresses.poke(addresses(first).U)
        dut.io.tcdmRequest.poke(511.U)
        dut.io.tcdmGrant.expect(511)
        dut.clock.step()
        dut.io.tcdmResponseValid.expect(511)
        val expected = (0 until 9).map(lane =>
          BigInt(first + lane + 1) << (32 * lane)).reduce(_ | _)
        dut.io.tcdmResponse.expect(expected)
      }
      dut.io.tcdmRequest.poke(0.U)
      dut.clock.step()
      dut.io.tcdmResponseValid.expect(0)
    }
  }

  it should "hold CPU data under backpressure without blocking NE16 or DMA" in {
    simulate(new NE16Scratchpad(params)) { dut =>
      initialize(dut)
      val expected = BigInt("8877665544332211", 16)
      write(dut, 0, 0, expected)
      dut.io.response(0).ready.poke(false.B)
      dut.io.request(0).bits.write.poke(false.B)
      dut.io.request(0).valid.poke(true.B)
      waitFor(dut.io.request(0).ready.peek().litToBoolean, dut)
      dut.clock.step()
      dut.io.request(0).valid.poke(false.B)
      dut.io.tcdmRequest.poke(511.U)
      dut.io.tcdmAddresses.poke(addresses(12).U)
      waitFor(dut.io.response(0).valid.peek().litToBoolean, dut)
      for (_ <- 0 until 8) {
        dut.io.response(0).valid.expect(true.B)
        dut.io.response(0).bits.expect(expected)
        dut.io.tcdmGrant.expect(511)
        dut.clock.step()
      }
      write(dut, 1, 80, BigInt("1020304050607080", 16), 0x3c)
      dut.io.response(0).bits.expect(expected)
      dut.io.response(0).ready.poke(true.B)
      dut.clock.step()
      dut.io.response(0).valid.expect(false.B)
    }
  }

  it should "apply wide byte masks atomically across all banks" in {
    simulate(new NE16Scratchpad(params)) { dut =>
      initialize(dut)
      for (word <- 0 until 20 by 2) {
        write(dut, 0, word * 4, BigInt("aaaaaaaaaaaaaaaa", 16))
      }
      dut.io.tcdmAddresses.poke(addresses(5).U)
      dut.io.tcdmRead.poke(0.U)
      dut.io.tcdmData.poke((0 until 9).map(lane =>
        BigInt("44332211", 16) << (32 * lane)).reduce(_ | _).U)
      dut.io.tcdmMask.poke((0 until 9).map(lane =>
        BigInt(5) << (4 * lane)).reduce(_ | _).U)
      dut.io.tcdmRequest.poke(511.U)
      dut.clock.step()
      dut.io.tcdmRead.poke(511.U)
      dut.clock.step()
      dut.io.tcdmResponseValid.expect(511)
      dut.io.tcdmResponse.expect((0 until 9).map(lane =>
        BigInt("aa33aa11", 16) << (32 * lane)).reduce(_ | _))
    }
  }
}
