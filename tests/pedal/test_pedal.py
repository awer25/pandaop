import time
import unittest
from panda import Panda
from panda_jungle import PandaJungle  # pylint: disable=import-error


class CanHandle(object):
  def __init__(self, p):
    self.p = p

  def transact(self, dat):
    self.p.isotp_send(1, dat, 0, recvaddr=2)

    def _handle_timeout(signum, frame):
      # will happen on reset
      raise Exception("timeout")

    signal.signal(signal.SIGALRM, _handle_timeout)
    signal.alarm(1)
    try:
      ret = self.p.isotp_recv(2, 0, sendaddr=1)
    finally:
      signal.alarm(0)

    return ret

  def controlWrite(self, request_type, request, value, index, data, timeout=0):
    # ignore data in reply, panda doesn't use it
    return self.controlRead(request_type, request, value, index, 0, timeout)

  def controlRead(self, request_type, request, value, index, length, timeout=0):
    dat = struct.pack("HHBBHHH", 0, 0, request_type, request, value, index, length)
    return self.transact(dat)

  def bulkWrite(self, endpoint, data, timeout=0):
    if len(data) > 0x10:
      raise ValueError("Data must not be longer than 0x10")
    dat = struct.pack("HH", endpoint, len(data)) + data
    return self.transact(dat)

  def bulkRead(self, endpoint, length, timeout=0):
    dat = struct.pack("HH", endpoint, 0)
    return self.transact(dat)
    

class TestPedal(unittest.TestCase):
  PEDAL_BUS = 1
  def setUp(self):
    self.jungle = PandaJungle()
    self.jungle.set_panda_power(True)
    self.jungle.set_ignition(False)

  def tearDown(self):
    self.jungle.close()

  def _flash_over_can(self, bus, fw_file):
    print(f"Flashing {fw_file}")
    while len(self.jungle.can_recv()) != 0:
      continue
    self.jungle.can_send(0x200, b"\xce\xfa\xad\xde\x1e\x0b\xb0\x0a", bus)

    time.sleep(0.1)
    with open(fw_file, "rb") as code:
      PandaJungle.flash_static(CanHandle(self.jungle, bus), code.read())

  def test_aaa_flash_over_can(self):
    self._flash_over_can(self.PEDAL_BUS, "/tmp/pedal.bin.signed")
    time.sleep(10)
    pandas_list = Panda.list()

    self._flash_over_can(self.PEDAL_BUS, "/tmp/pedal_usb.bin.signed")
    time.sleep(10)
    pedal_uid = (set(Panda.list()) ^ set(pandas_list)).pop()

    p = Panda(pedal_uid)
    self.assertTrue(p.is_pedal())
    p.close()

  def test_can_spam(self):
    p = Panda('none')
    p.set_pedal_silent(False)
    p.close()

    self.jungle.can_clear(0xFFFF)
    rounds = 10
    msgs = 0
    while rounds > 0:
      incoming = self.jungle.can_recv()
      for message in incoming:
        address, _, _, bus = message
        if address == 0x201 and bus == self.PEDAL_BUS:
          msgs += 1
      time.sleep(0.1)
      rounds -= 1
    
    self.assertTrue(msgs > 40)
    print(f"Got {msgs} messages")

  def test_silent_mode(self):
    p = Panda('none')
    p.set_pedal_silent(True)
    p.close()

    self.jungle.can_clear(0xFFFF)
    rounds = 10
    msgs = 0
    while rounds > 0:
      incoming = self.jungle.can_recv()
      for message in incoming:
        address, _, _, bus = message
        if address == 0x201 and bus == self.PEDAL_BUS:
          msgs += 1
      time.sleep(0.1)
      rounds -= 1

    self.assertTrue(msgs == 0)
    print(f"Got {msgs} messages")


if __name__ == '__main__':
  unittest.main()
