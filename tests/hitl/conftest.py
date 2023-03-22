import concurrent.futures
import os
import time
import pytest

from panda import Panda
from panda_jungle import PandaJungle  # pylint: disable=import-error
from panda.tests.hitl.helpers import clear_can_buffers


SPEED_NORMAL = 500
SPEED_GMLAN = 33.3
BUS_SPEEDS = [(0, SPEED_NORMAL), (1, SPEED_NORMAL), (2, SPEED_NORMAL), (3, SPEED_GMLAN)]


class PandaGroup:
  H7 = (Panda.HW_TYPE_RED_PANDA, Panda.HW_TYPE_RED_PANDA_V2)
  GEN2 = (Panda.HW_TYPE_BLACK_PANDA, Panda.HW_TYPE_UNO) + H7
  GPS = (Panda.HW_TYPE_BLACK_PANDA, Panda.HW_TYPE_UNO)
  GMLAN = (Panda.HW_TYPE_WHITE_PANDA, Panda.HW_TYPE_GREY_PANDA)

PEDAL_SERIAL = 'none'
JUNGLE_SERIAL = os.getenv("PANDAS_JUNGLE")
PANDAS_EXCLUDE = os.getenv("PANDAS_EXCLUDE", "").strip().split(" ")
PARTIAL_TESTS = os.environ.get("PARTIAL_TESTS", "0") == "1"

# Find all pandas connected
_all_pandas = {}
panda_jungle = None
def init_all_pandas():
  global _all_pandas
  global panda_jungle

  panda_jungle = PandaJungle(JUNGLE_SERIAL)
  panda_jungle.set_panda_power(True)

  for serial in Panda.list():
    if serial not in PANDAS_EXCLUDE and serial != PEDAL_SERIAL:
      with Panda(serial=serial) as p:
        _all_pandas[serial] = p.get_type()
  print(f"{len(_all_pandas)} total pandas")
init_all_pandas()
_all_panda_serials = list(_all_pandas.keys())



def pytest_configure(config):
  config.addinivalue_line(
    "markers", "test_panda_types(name): mark test to run only on specified panda types"
  )


def pytest_make_parametrize_id(config, val, argname):
  if val in _all_pandas:
    # TODO: get nice string instead of int
    hw_type = _all_pandas[val][0]
    return f"serial={val}, hw_type={hw_type}"
  return None


@pytest.fixture(name='p', params=_all_panda_serials)
def fixture_panda(request):
  panda_serial = request.param

  mark = request.node.get_closest_marker('test_panda_types')
  if mark:
    assert len(mark.args) > 0, "Missing allowed panda types in mark"
    test_types = mark.args[0]
    if _all_pandas[panda_serial] not in test_types:
      pytest.skip(f"Not applicable, {test_types} pandas only")

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
    if s == panda_serial:
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

  # run test
  yield pandas[0]

  # teardown
  try:
    # Check if the pandas did not throw any faults while running test
    for panda in pandas:
      if not panda.bootstub:
        #panda.reconnect()
        assert panda.health()['fault_status'] == 0
        # Check health of each CAN core after test, normal to fail for test_gen2_loopback on OBD bus, so skipping
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
