#!/usr/bin/env python3
import unittest
import numpy as np
from panda import Panda
from panda.tests.libpanda import libpanda_py
import panda.tests.safety.common as common
from panda.tests.safety.common import CANPackerPanda

MSG_DAS_steeringControl = 0x488
MSG_DAS_Control = 0x2b9
MSG_SCCM_rightStalk = 0x229

class TestTeslaSafetyBase(common.PandaCarSafetyTest):
  RELAY_MALFUNCTION_ADDRS = {0: (MSG_DAS_steeringControl,)}
  FWD_BLACKLISTED_ADDRS = {2: [MSG_DAS_steeringControl]}
  TX_MSGS = [[MSG_DAS_steeringControl, 0], [MSG_DAS_Control, 0], [MSG_SCCM_rightStalk, 1]]

  STANDSTILL_THRESHOLD = 1
  GAS_PRESSED_THRESHOLD = 3
  FWD_BUS_LOOKUP = {0: 2, 2: 0}

  packer: CANPackerPanda
  packer_vehicle: CANPackerPanda

  @classmethod
  def setUpClass(cls):
    if cls.__name__ == "TestTeslaSafetyBase":
      raise unittest.SkipTest

  def _user_brake_msg(self, brake):
    values = {"IBST_driverBrakeApply": 2 if brake else 1}
    return self.packer.make_can_msg_panda("IBST_status", 0, values)

  def _speed_msg(self, speed):
    values = {"ESP_vehicleSpeed": speed / 0.277778}
    return self.packer.make_can_msg_panda("ESP_B", 0, values)

  def _vehicle_moving_msg(self, speed: float):
    values = {"ESP_vehicleStandstillSts": 1 if speed <= self.STANDSTILL_THRESHOLD else 0}
    return self.packer.make_can_msg_panda("ESP_B", 0, values)

  def _user_gas_msg(self, gas):
    values = {"DI_accelPedalPos": gas}
    return self.packer.make_can_msg_panda("DI_systemStatus", 0, values)

  def _pcm_status_msg(self, enable):
    values = {"DI_cruiseState": 2 if enable else 0}
    return self.packer.make_can_msg_panda("DI_state", 0, values)

  def _control_lever_cmd(self, command):
    values = {"SCCM_rightStalkStatus": command}
    return self.packer_vehicle.make_can_msg_panda("SCCM_rightStalk", 1, values)

  def _long_control_msg(self, set_speed, acc_val=0, jerk_limits=(0, 0), accel_limits=(0, 0), aeb_event=0, bus=0):
    values = {
      "DAS_setSpeed": set_speed,
      "DAS_accState": acc_val,
      "DAS_aebEvent": aeb_event,
      "DAS_jerkMin": jerk_limits[0],
      "DAS_jerkMax": jerk_limits[1],
      "DAS_accelMin": accel_limits[0],
      "DAS_accelMax": accel_limits[1],
    }
    return self.packer.make_can_msg_panda("DAS_control", bus, values)

class TestTeslaStockSafety(TestTeslaSafetyBase):
  # Angle control limits
  DEG_TO_CAN = 10

  ANGLE_RATE_BP = [0., 5., 15.]
  ANGLE_RATE_UP = [10., 1.6, .3]  # windup limit
  ANGLE_RATE_DOWN = [10., 7.0, .8]  # unwind limit

  def setUp(self):
    self.packer = CANPackerPanda("tesla_model3_party")
    self.packer_vehicle = CANPackerPanda("tesla_model3_vehicle")
    self.safety = libpanda_py.libpanda
    self.safety.set_safety_hooks(Panda.SAFETY_TESLA, 0)
    self.safety.init_tests()

  def _angle_cmd_msg(self, angle: float, enabled: bool):
    values = {"DAS_steeringAngleRequest": angle, "DAS_steeringControlType": 1 if enabled else 0}
    return self.packer.make_can_msg_panda("DAS_steeringControl", 0, values)

  def _angle_meas_msg(self, angle: float):
    values = {"EPAS3S_internalSAS": angle}
    return self.packer.make_can_msg_panda("EPAS3S_sysStatus", 0, values)

  def test_acc_buttons(self):
    """
      cancel & idle allowed.
    """
    btns = [
      (0, True),  # IDLE
      (1, True),  # HALF UP
      (2, False),  # FULL UP
      (3, False),  # HALF DOWN
      (4, False),  # FULL DOWN
    ]
    for btn, should_tx in btns:
      for controls_allowed in (True, False):
        self.safety.set_controls_allowed(controls_allowed)
        tx = self._tx(self._control_lever_cmd(btn))
        self.assertEqual(tx, should_tx)

class TestTeslaLongitudinalSafetyBase(TestTeslaSafetyBase):
  RELAY_MALFUNCTION_ADDRS = {0: (MSG_DAS_steeringControl, MSG_DAS_Control)}
  FWD_BLACKLISTED_ADDRS = {2: [MSG_DAS_steeringControl, MSG_DAS_Control]}

  MAX_ACCEL = 2.2
  MIN_ACCEL = -5.12
  INACTIVE_ACCEL = 0.0

  def setUp(self):
    self.packer = CANPackerPanda("tesla_model3_party")
    self.packer_vehicle = CANPackerPanda("tesla_model3_vehicle")
    self.safety = libpanda_py.libpanda
    self.safety.set_safety_hooks(Panda.SAFETY_TESLA, Panda.FLAG_TESLA_LONG_CONTROL)
    self.safety.init_tests()

  def test_no_aeb(self):
    for aeb_event in range(4):
      self.assertEqual(self._tx(self._long_control_msg(10, aeb_event=aeb_event)), aeb_event == 0)

  def test_stock_aeb_passthrough(self):
    no_aeb_msg = self._long_control_msg(10, aeb_event=0)
    no_aeb_msg_cam = self._long_control_msg(10, aeb_event=0, bus=2)
    aeb_msg_cam = self._long_control_msg(10, aeb_event=1, bus=2)

    # stock system sends no AEB -> no forwarding, and OP is allowed to TX
    self.assertEqual(1, self._rx(no_aeb_msg_cam))
    self.assertEqual(-1, self.safety.safety_fwd_hook(2, no_aeb_msg_cam.addr))
    self.assertTrue(self._tx(no_aeb_msg))

    # stock system sends AEB -> forwarding, and OP is not allowed to TX
    self.assertEqual(1, self._rx(aeb_msg_cam))
    self.assertEqual(0, self.safety.safety_fwd_hook(2, aeb_msg_cam.addr))
    self.assertFalse(self._tx(no_aeb_msg))


if __name__ == "__main__":
  unittest.main()
