from panda import Panda, PandaDFU
from .helpers import test_all_pandas, panda_connect_and_init

@test_all_pandas
@panda_connect_and_init
def test_dfu(p):
  app_mcu_type = p.get_mcu_type()
  dfu_serial = PandaDFU.st_serial_to_dfu_serial(p.get_usb_serial(), p.get_mcu_type())

  p.reset(enter_bootstub=True)
  p.reset(enter_bootloader=True)
  assert Panda.wait_for_dfu(dfu_serial, timeout=20), "failed to enter DFU"

  dfu = PandaDFU(dfu_serial)
  assert dfu._mcu_type == app_mcu_type

  assert dfu_serial in PandaDFU.list()

  dfu.reset()
  p.reconnect()
