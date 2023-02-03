import concurrent.futures
import os
import time
import random
import faulthandler
from functools import wraps, partial
from nose.tools import assert_equal
from parameterized import parameterized

from panda import Panda
from panda_jungle import PandaJungle  # pylint: disable=import-error

SPEED_NORMAL = 500
SPEED_GMLAN = 33.3
BUS_SPEEDS = [(0, SPEED_NORMAL), (1, SPEED_NORMAL), (2, SPEED_NORMAL), (3, SPEED_GMLAN)]
H7_HW_TYPES = [Panda.HW_TYPE_RED_PANDA, Panda.HW_TYPE_RED_PANDA_V2]
GEN2_HW_TYPES = [Panda.HW_TYPE_BLACK_PANDA, Panda.HW_TYPE_UNO] + H7_HW_TYPES
GPS_HW_TYPES = [Panda.HW_TYPE_BLACK_PANDA, Panda.HW_TYPE_UNO]
PEDAL_SERIAL = 'none'
JUNGLE_SERIAL = os.getenv("PANDAS_JUNGLE")
PANDAS_EXCLUDE = os.getenv("PANDAS_EXCLUDE", "").strip().split(" ")

PARTIAL_TESTS = os.environ.get("PARTIAL_TESTS", "0") == "1"

# Enable fault debug
faulthandler.enable(all_threads=False)

# Connect to Panda Jungle
panda_jungle = PandaJungle(JUNGLE_SERIAL)

# Find all pandas connected
_all_pandas = []
def init_all_pandas():
  global _all_pandas
  _all_pandas = []

  # power cycle pandas
  panda_jungle.set_panda_power(False)
  time.sleep(3)
  panda_jungle.set_panda_power(True)
  time.sleep(5)

  for serial in Panda.list():
    if serial not in PANDAS_EXCLUDE and serial != PEDAL_SERIAL:
      with Panda(serial=serial) as p:
        _all_pandas.append((serial, p.get_type()))
  print(f"{len(_all_pandas)} total pandas")
init_all_pandas()
_all_panda_serials = [x[0] for x in _all_pandas]

def parameterized_panda_types(types):
  serials = []
  for typ in types:
    for s, t in _all_pandas:
      if t == typ and s not in serials:
        serials.append(s)
        break
    else:
      raise IOError("No unused panda found for type: {}".format(typ))
  return parameterized(serials)

# Panda providers
TESTED_HW_TYPES = (Panda.HW_TYPE_WHITE_PANDA, Panda.HW_TYPE_BLACK_PANDA, Panda.HW_TYPE_RED_PANDA, Panda.HW_TYPE_RED_PANDA_V2, Panda.HW_TYPE_UNO)
test_all_pandas = parameterized_panda_types(TESTED_HW_TYPES)
test_all_gen2_pandas = parameterized_panda_types(GEN2_HW_TYPES)
test_all_gps_pandas = parameterized_panda_types(GPS_HW_TYPES)

# no grey for speedup, should be sufficiently covered by white for these tests
test_all_gmlan_pandas = parameterized_panda_types([Panda.HW_TYPE_WHITE_PANDA, ])

if PARTIAL_TESTS:
  # minimal set of pandas to get most of our coverage
  # * red panda covers STM32H7
  # * black panda covers STM32F4, GEN2, and GPS
  partial_pandas = (Panda.HW_TYPE_BLACK_PANDA, Panda.HW_TYPE_RED_PANDA)
  test_all_pandas = parameterized_panda_types(partial_pandas)
  test_all_gen2_pandas = parameterized_panda_types(partial_pandas)
  test_all_gps_pandas = parameterized_panda_types([Panda.HW_TYPE_BLACK_PANDA, ])


def time_many_sends(p, bus, p_recv=None, msg_count=100, msg_id=None, two_pandas=False):
  if p_recv is None:
    p_recv = p
  if msg_id is None:
    msg_id = random.randint(0x100, 0x200)
  if p == p_recv and two_pandas:
    raise ValueError("Cannot have two pandas that are the same panda")

  start_time = time.monotonic()
  p.can_send_many([(msg_id, 0, b"\xaa" * 8, bus)] * msg_count)
  r = []
  r_echo = []
  r_len_expected = msg_count if two_pandas else msg_count * 2
  r_echo_len_exected = msg_count if two_pandas else 0

  while len(r) < r_len_expected and (time.monotonic() - start_time) < 5:
    r.extend(p_recv.can_recv())
  end_time = time.monotonic()
  if two_pandas:
    while len(r_echo) < r_echo_len_exected and (time.monotonic() - start_time) < 10:
      r_echo.extend(p.can_recv())

  sent_echo = [x for x in r if x[3] == 0x80 | bus and x[0] == msg_id]
  sent_echo.extend([x for x in r_echo if x[3] == 0x80 | bus and x[0] == msg_id])
  resp = [x for x in r if x[3] == bus and x[0] == msg_id]

  leftovers = [x for x in r if (x[3] != 0x80 | bus and x[3] != bus) or x[0] != msg_id]
  assert_equal(len(leftovers), 0)

  assert_equal(len(resp), msg_count)
  assert_equal(len(sent_echo), msg_count)

  end_time = (end_time - start_time) * 1000.0
  comp_kbps = (1 + 11 + 1 + 1 + 1 + 4 + 8 * 8 + 15 + 1 + 1 + 1 + 7) * msg_count / end_time

  return comp_kbps

def panda_connect_and_init(fn=None):
  if not fn:
    return partial(panda_connect_and_init)

  @wraps(fn)
  def wrapper(panda_serials, **kwargs):
    # Change panda_serials to a list
    if panda_serials is not None:
      if not isinstance(panda_serials, list):
        panda_serials = [panda_serials, ]

    # Initialize jungle
    clear_can_buffers(panda_jungle)
    panda_jungle.set_panda_power(True)
    panda_jungle.set_can_loopback(False)
    panda_jungle.set_obd(False)
    panda_jungle.set_harness_orientation(PandaJungle.HARNESS_ORIENTATION_1)
    for bus, speed in BUS_SPEEDS:
      panda_jungle.set_can_speed_kbps(bus, speed)

    # wait for all pandas to come up
    for _ in range(50):
      if set(_all_panda_serials).issubset(set(Panda.list())):
        break
      time.sleep(0.1)

    # Connect to pandas
    def cnnct(s):
      if s in panda_serials:
        p = Panda(serial=s)
        p.reset(reconnect=True)

        p.set_can_loopback(False)
        p.set_gmlan(None)
        p.set_esp_power(False)
        p.set_power_save(False)
        for bus, speed in BUS_SPEEDS:
          p.set_can_speed_kbps(bus, speed)
        clear_can_buffers(p)
        p.set_power_save(False)
        return p
      else:
        with Panda(serial=s) as p:
          p.reset(reconnect=False)
      return None

    with concurrent.futures.ThreadPoolExecutor() as exc:
      ps = list(exc.map(cnnct, _all_panda_serials, timeout=20))
      pandas = [p for p in ps if p is not None]

    try:
      fn(*pandas, *kwargs)

      # Check if the pandas did not throw any faults while running test
      for panda in pandas:
        if not panda.bootstub:
          #panda.reconnect()
          assert panda.health()['fault_status'] == 0
          # Check health of each CAN core after test, normal to fail for test_gen2_loopback on OBD bus, so skipping
          if fn.__name__ != "test_gen2_loopback":
            for i in range(3):
              can_health = panda.can_health(i)
              assert can_health['bus_off_cnt'] == 0
              assert can_health['receive_error_cnt'] == 0
              assert can_health['transmit_error_cnt'] == 0
              assert can_health['total_rx_lost_cnt'] == 0
              assert can_health['total_tx_lost_cnt'] == 0
              assert can_health['total_error_cnt'] == 0
              assert can_health['total_tx_checksum_error_cnt'] == 0
    finally:
      for p in pandas:
        try:
          p.close()
        except Exception:
          pass
  return wrapper

def clear_can_buffers(panda):
  # clear tx buffers
  for i in range(4):
    panda.can_clear(i)

  # clear rx buffers
  panda.can_clear(0xFFFF)
  r = [1]
  st = time.monotonic()
  while len(r) > 0:
    r = panda.can_recv()
    time.sleep(0.05)
    if (time.monotonic() - st) > 10:
      print("Unable to clear can buffers for panda ", panda.get_serial())
      assert False

def check_signature(p):
  assert not p.bootstub, "Flashed firmware not booting. Stuck in bootstub."
  firmware_sig = Panda.get_signature_from_firmware(p.get_mcu_type().config.app_path)
  panda_sig = p.get_signature()
  assert_equal(panda_sig, firmware_sig)
