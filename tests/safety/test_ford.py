#!/usr/bin/env python3
import time
import numpy as np
import unittest

import panda.tests.safety.common as common

from panda import Panda
from panda.tests.libpanda import libpanda_py
from panda.tests.safety.common import CANPackerPanda

MSG_EngBrakeData = 0x165           # RX from PCM, for driver brake pedal and cruise state
MSG_EngVehicleSpThrottle = 0x204   # RX from PCM, for driver throttle input
MSG_BrakeSysFeatures = 0x415       # RX from ABS, for vehicle speed
MSG_EngVehicleSpThrottle2 = 0x202  # RX from PCM, for second vehicle speed
MSG_Yaw_Data_FD1 = 0x91            # RX from RCM, for yaw rate
MSG_Steering_Data_FD1 = 0x083      # TX by OP, various driver switches and LKAS/CC buttons
MSG_ACCDATA_3 = 0x18A              # TX by OP, ACC/TJA user interface
MSG_Lane_Assist_Data1 = 0x3CA      # TX by OP, Lane Keep Assist
MSG_LateralMotionControl = 0x3D3   # TX by OP, Traffic Jam Assist
MSG_IPMA_Data = 0x3D8              # TX by OP, IPMA and LKAS user interface


def checksum(msg):
  addr, t, dat, bus = msg
  ret = bytearray(dat)

  if addr == MSG_Yaw_Data_FD1:
    chksum = dat[0] + dat[1]  # VehRol_W_Actl
    chksum += dat[2] + dat[3]  # VehYaw_W_Actl
    chksum += dat[5]  # VehRollYaw_No_Cnt
    chksum += dat[6] >> 6  # VehRolWActl_D_Qf
    chksum += (dat[6] >> 4) & 0x3  # VehYawWActl_D_Qf
    chksum = 0xff - (chksum & 0xff)
    ret[4] = chksum

  elif addr == MSG_BrakeSysFeatures:
    chksum = dat[0] + dat[1]  # Veh_V_ActlBrk
    chksum += (dat[2] >> 2) & 0xf  # VehVActlBrk_No_Cnt
    chksum += dat[2] >> 6  # VehVActlBrk_D_Qf
    chksum = 0xff - (chksum & 0xff)
    ret[3] = chksum

  elif addr == MSG_EngVehicleSpThrottle2:
    chksum = (dat[2] >> 3) & 0xf  # VehVActlEng_No_Cnt
    chksum += (dat[4] >> 5) & 0x3  # VehVActlEng_D_Qf
    chksum += dat[6] + dat[7]  # Veh_V_ActlEng
    chksum = 0xff - (chksum & 0xff)
    ret[1] = chksum

  return addr, t, ret, bus


def round_curvature_can(curvature):
  # rounds curvature as if it was sent on CAN
  return round(round(curvature * 5, 4) / 5, 5)


def round_curvature_can_2(curvature):
  # rounds curvature as if it was sent on CAN
  return round(round((curvature + 0.02) / 2e-5) * 2e-5 - 0.02, 5)


class Buttons:
  CANCEL = 0
  RESUME = 1
  TJA_TOGGLE = 2


class TestFordSafety(common.PandaSafetyTest,
                     # common.CurvatureSteeringSafetyTest
                     ):
  STANDSTILL_THRESHOLD = 1
  RELAY_MALFUNCTION_ADDR = MSG_IPMA_Data
  RELAY_MALFUNCTION_BUS = 0

  TX_MSGS = [
    [MSG_Steering_Data_FD1, 0], [MSG_Steering_Data_FD1, 2], [MSG_ACCDATA_3, 0], [MSG_Lane_Assist_Data1, 0],
    [MSG_LateralMotionControl, 0], [MSG_IPMA_Data, 0],
  ]
  FWD_BLACKLISTED_ADDRS = {2: [MSG_ACCDATA_3, MSG_Lane_Assist_Data1, MSG_LateralMotionControl, MSG_IPMA_Data]}
  FWD_BUS_LOOKUP = {0: 2, 2: 0}

  # Max allowed delta between car speeds
  MAX_SPEED_DELTA = 2.0  # m/s

  # Curvature control limits
  DEG_TO_CAN = 50000  # 1 / (2e-5) rad to can
  MAX_CURVATURE = 0.02
  MAX_CURVATURE_DELTA = 0.002
  CURVATURE_DELTA_LIMIT_SPEED = 10.0  # m/s

  # todo new stuff
  HIGH_ANGLE = 0.014  # 0.02 - ANGLE_DELTA_VU[0] = 0.014
  ANGLE_RATE_VIOLATION_OFFSET = 0.001

  ANGLE_DELTA_BP = [5., 25., 25.]
  ANGLE_DELTA_V = [0.0002, 0.0001, 0.0001]  # windup limit
  ANGLE_DELTA_VU = [0.000225, 0.00015, 0.00015]  # unwind limit

  cnt_speed = 0
  cnt_speed_2 = 0
  cnt_yaw_rate = 0

  def setUp(self):
    self.packer = CANPackerPanda("ford_lincoln_base_pt")
    self.safety = libpanda_py.libpanda
    self.safety.set_safety_hooks(Panda.SAFETY_FORD, 0)
    self.safety.init_tests()

  def _set_prev_desired_angle(self, t):
    t = round(t * self.DEG_TO_CAN)
    self.safety.set_desired_angle_last(t)

  def _reset_curvature_measurement(self, curvature, speed):
    self._rx(self._speed_msg(speed))
    for _ in range(6):
      self._rx(self._yaw_rate_msg(curvature, speed))

  # Angle cmd is TJA message with only curvature
  def _angle_cmd_msg(self, angle: float, enabled: bool):
    return self._tja_command_msg(enabled, 0, 0, angle, 0)

  # Driver brake pedal
  def _user_brake_msg(self, brake: bool):
    # brake pedal and cruise state share same message, so we have to send
    # the other signal too
    enable = self.safety.get_controls_allowed()
    values = {
      "BpedDrvAppl_D_Actl": 2 if brake else 1,
      "CcStat_D_Actl": 5 if enable else 0,
    }
    return self.packer.make_can_msg_panda("EngBrakeData", 0, values)

  # ABS vehicle speed
  def _speed_msg(self, speed: float, quality_flag=True):
    values = {"Veh_V_ActlBrk": speed * 3.6, "VehVActlBrk_D_Qf": 3 if quality_flag else 0, "VehVActlBrk_No_Cnt": self.cnt_speed % 16}
    self.__class__.cnt_speed += 1
    return self.packer.make_can_msg_panda("BrakeSysFeatures", 0, values, fix_checksum=checksum)

  # PCM vehicle speed
  def _speed_msg_2(self, speed: float, quality_flag=True):
    values = {"Veh_V_ActlEng": speed * 3.6, "VehVActlEng_D_Qf": 3 if quality_flag else 0, "VehVActlEng_No_Cnt": self.cnt_speed_2 % 16}
    self.__class__.cnt_speed_2 += 1
    return self.packer.make_can_msg_panda("EngVehicleSpThrottle2", 0, values, fix_checksum=checksum)

  # Standstill state
  def _vehicle_moving_msg(self, speed: float):
    values = {"VehStop_D_Stat": 1 if speed <= self.STANDSTILL_THRESHOLD else 0}
    return self.packer.make_can_msg_panda("DesiredTorqBrk", 0, values)

  # Current curvature
  def _yaw_rate_msg(self, curvature: float, speed: float, quality_flag=True):
    values = {"VehYaw_W_Actl": curvature * speed, "VehYawWActl_D_Qf": 3 if quality_flag else 0,
              "VehRolWActl_D_Qf": 3 if quality_flag else 0, "VehRollYaw_No_Cnt": self.cnt_yaw_rate % 256}
    self.__class__.cnt_yaw_rate += 1
    return self.packer.make_can_msg_panda("Yaw_Data_FD1", 0, values, fix_checksum=checksum)

  # Drive throttle input
  def _user_gas_msg(self, gas: float):
    values = {"ApedPos_Pc_ActlArb": gas}
    return self.packer.make_can_msg_panda("EngVehicleSpThrottle", 0, values)

  # Cruise status
  def _pcm_status_msg(self, enable: bool):
    # brake pedal and cruise state share same message, so we have to send
    # the other signal too
    brake = self.safety.get_brake_pressed_prev()
    values = {
      "BpedDrvAppl_D_Actl": 2 if brake else 1,
      "CcStat_D_Actl": 5 if enable else 0,
    }
    return self.packer.make_can_msg_panda("EngBrakeData", 0, values)

  # LKAS command
  def _lkas_command_msg(self, action: int):
    values = {
      "LkaActvStats_D2_Req": action,
    }
    return self.packer.make_can_msg_panda("Lane_Assist_Data1", 0, values)

  # TJA command
  def _tja_command_msg(self, enabled: bool, path_offset: float, path_angle: float, curvature: float, curvature_rate: float):
    values = {
      "LatCtl_D_Rq": 1 if enabled else 0,
      "LatCtlPathOffst_L_Actl": path_offset,     # Path offset [-5.12|5.11] meter
      "LatCtlPath_An_Actl": path_angle,          # Path angle [-0.5|0.5235] radians
      "LatCtlCurv_NoRate_Actl": curvature_rate,  # Curvature rate [-0.001024|0.00102375] 1/meter^2
      "LatCtlCurv_No_Actl": curvature,           # Curvature [-0.02|0.02094] 1/meter
    }
    return self.packer.make_can_msg_panda("LateralMotionControl", 0, values)

  # Cruise control buttons
  def _acc_button_msg(self, button: int, bus: int):
    values = {
      "CcAslButtnCnclPress": 1 if button == Buttons.CANCEL else 0,
      "CcAsllButtnResPress": 1 if button == Buttons.RESUME else 0,
      "TjaButtnOnOffPress": 1 if button == Buttons.TJA_TOGGLE else 0,
    }
    return self.packer.make_can_msg_panda("Steering_Data_FD1", bus, values)

  def test_rx_hook(self):
    # checksum, counter, and quality flag checks
    for quality_flag in [True, False]:
      for msg in ["speed", "speed_2", "yaw"]:
        self.safety.set_controls_allowed(True)
        # send multiple times to verify counter checks
        for _ in range(10):
          if msg == "speed":
            to_push = self._speed_msg(0, quality_flag=quality_flag)
          elif msg == "speed_2":
            to_push = self._speed_msg_2(0, quality_flag=quality_flag)
          elif msg == "yaw":
            to_push = self._yaw_rate_msg(0, 0, quality_flag=quality_flag)

          self.assertEqual(quality_flag, self._rx(to_push))
          self.assertEqual(quality_flag, self.safety.get_controls_allowed())

        # Mess with checksum to make it fail
        to_push[0].data[1] = 0  # Speed 2 checksum
        to_push[0].data[3] = 0  # Speed checksum & half of yaw signal
        self.assertFalse(self._rx(to_push))
        self.assertFalse(self.safety.get_controls_allowed())

  # def test_rx_hook_speed_mismatch(self):
  #   # Ford relies on speed for driver curvature limiting, so it checks two sources
  #   for speed in np.arange(0, 40, 1):
  #     for speed_delta in np.arange(-5, 5, 0.1):
  #       speed_2 = round(max(speed + speed_delta, 0), 1)
  #       # Set controls allowed in between rx since first message can reset it
  #       self._rx(self._speed_msg(speed))
  #       self.safety.set_controls_allowed(True)
  #       self._rx(self._speed_msg_2(speed_2))
  #
  #       within_delta = abs(speed - speed_2) <= self.MAX_SPEED_DELTA
  #       self.assertEqual(self.safety.get_controls_allowed(), within_delta)

  # def test_steer_allowed(self):
  #   path_offsets = np.arange(-5.12, 5.11, 1).round()
  #   path_angles = np.arange(-0.5, 0.5235, 0.1).round(1)
  #   curvature_rates = np.arange(-0.001024, 0.00102375, 0.001).round(3)
  #   curvatures = np.arange(-0.02, 0.02094, 0.01).round(2)
  #
  #   for controls_allowed in (True, False):
  #     for steer_control_enabled in (True, False):
  #       for path_offset in path_offsets:
  #         for path_angle in path_angles:
  #           for curvature_rate in curvature_rates:
  #             for curvature in curvatures:
  #               self.safety.set_controls_allowed(controls_allowed)
  #               self._set_prev_desired_angle(curvature)
  #
  #               should_tx = path_offset == 0 and path_angle == 0 and curvature_rate == 0
  #               # when request bit is 0, only allow curvature of 0 since the signal range
  #               # is not large enough to enforce it tracking measured
  #               should_tx = should_tx and (controls_allowed if steer_control_enabled else curvature == 0)
  #               with self.subTest(controls_allowed=controls_allowed, steer_control_enabled=steer_control_enabled,
  #                                 path_offset=path_offset, path_angle=path_angle, curvature_rate=curvature_rate,
  #                                 curvature=curvature):
  #                 self.assertEqual(should_tx, self._tx(self._tja_command_msg(steer_control_enabled, path_offset, path_angle, curvature, curvature_rate)))

  # def test_curvature_rate_limits(self):
  #   speed = 20
  #   s = speed
  #
  #   for initial_curvature in np.linspace(-self.MAX_CURVATURE, self.MAX_CURVATURE, 21):
  #     max_delta_up = np.interp(s, self.ANGLE_DELTA_BP, self.ANGLE_DELTA_V)
  #     max_delta_down = np.interp(s, self.ANGLE_DELTA_BP, self.ANGLE_DELTA_VU)
  #
  #     print(f'\n{speed=}, {max_delta_up=}, {max_delta_down=}')

  def test_curvature_rate_limit_up(self):
    self.safety.set_controls_allowed(True)
    small_curvature = 2 / self.DEG_TO_CAN  # significant small amount of curvature to cross boundary
    curvature_meas = self.MAX_CURVATURE_DELTA + 1e-3

    for speed in np.arange(0, 41, 5):
      limit_command = speed > self.CURVATURE_DELTA_LIMIT_SPEED
      max_delta_up_upper = np.interp(speed, self.ANGLE_DELTA_BP, self.ANGLE_DELTA_VU)
      max_delta_up_lower = np.interp(speed + 1, self.ANGLE_DELTA_BP, self.ANGLE_DELTA_VU)

      for sign in (-1, 1):
        self._reset_curvature_measurement(sign * curvature_meas, speed)

        # stay the same
        self._set_prev_desired_angle(0)
        self.assertEqual(not limit_command,
                         self._tx(self._tja_command_msg(True, 0, 0, 0, 0)))

        # ramp up under min allowed rate
        self._set_prev_desired_angle(0)
        self.assertEqual(not limit_command,
                         self._tx(self._tja_command_msg(True, 0, 0, sign * (max_delta_up_lower - small_curvature), 0)))

        # ramp up at minimum max rate
        self._set_prev_desired_angle(0)
        self.assertTrue(self._tx(self._tja_command_msg(True, 0, 0, sign * max_delta_up_lower, 0)))

        # ramp up at max rate
        self._set_prev_desired_angle(0)
        self.assertTrue(self._tx(self._tja_command_msg(True, 0, 0, sign * max_delta_up_upper, 0)))

        # ramp up above max rate
        self._set_prev_desired_angle(0)
        self.assertFalse(self._tx(self._tja_command_msg(True, 0, 0, sign * (max_delta_up_upper + small_curvature), 0)))

  def test_curvature_rate_limit_down(self):
    self.safety.set_controls_allowed(True)
    small_curvature = 2 / self.DEG_TO_CAN  # significant small amount of curvature to cross boundary
    curvature_meas = self.MAX_CURVATURE - self.MAX_CURVATURE_DELTA - 1e-3

    for speed in np.arange(0, 41, 5):
      limit_command = speed > self.CURVATURE_DELTA_LIMIT_SPEED
      max_delta_down_upper = np.interp(speed, self.ANGLE_DELTA_BP, self.ANGLE_DELTA_VU)
      max_delta_down_lower = np.interp(speed + 1, self.ANGLE_DELTA_BP, self.ANGLE_DELTA_VU)

      for sign in (-1, 1):
        self._reset_curvature_measurement(sign * curvature_meas, speed)

        # stay the same
        self._set_prev_desired_angle(sign * self.MAX_CURVATURE)
        self.assertEqual(not limit_command,
                         self._tx(self._tja_command_msg(True, 0, 0, sign * self.MAX_CURVATURE, 0)))

        # ramp down under min allowed rate
        self._set_prev_desired_angle(sign * self.MAX_CURVATURE)
        curvature = self.MAX_CURVATURE - max_delta_down_lower + small_curvature
        self.assertEqual(not limit_command,
                         self._tx(self._tja_command_msg(True, 0, 0, sign * curvature, 0)))

        # ramp down at minimum max rate
        self._set_prev_desired_angle(sign * self.MAX_CURVATURE)
        curvature = self.MAX_CURVATURE - max_delta_down_lower
        self.assertTrue(self._tx(self._tja_command_msg(True, 0, 0, sign * curvature, 0)))

        # ramp down at max rate
        self._set_prev_desired_angle(sign * self.MAX_CURVATURE)
        curvature = self.MAX_CURVATURE - max_delta_down_upper
        self.assertTrue(self._tx(self._tja_command_msg(True, 0, 0, sign * curvature, 0)))

        # ramp down above max rate
        self._set_prev_desired_angle(sign * self.MAX_CURVATURE)
        curvature = self.MAX_CURVATURE - max_delta_down_upper - small_curvature
        self.assertFalse(self._tx(self._tja_command_msg(True, 0, 0, sign * curvature, 0)))

  # def test_steer_meas_delta(self):
  #   """This safety model enforces a maximum distance from measured and commanded curvature, only above a certain speed"""
  #   self.safety.set_controls_allowed(1)
  #
  #   for steer_control_enabled in (True, False):
  #     for speed in np.linspace(0, 50, 11):
  #       for initial_curvature in np.linspace(-self.MAX_CURVATURE, self.MAX_CURVATURE, 21):
  #         self._reset_curvature_measurement(initial_curvature, speed)
  #
  #         limit_command = speed > self.CURVATURE_DELTA_LIMIT_SPEED
  #         for new_curvature in np.linspace(-self.MAX_CURVATURE, self.MAX_CURVATURE, 41):
  #           self._set_prev_desired_angle(new_curvature)
  #
  #           if steer_control_enabled:
  #             too_far_away = round_curvature_can(abs(new_curvature - initial_curvature)) > self.MAX_CURVATURE_DELTA
  #             should_tx = not limit_command or not too_far_away
  #           else:
  #             # enforce angle error limit is disabled when steer request bit is 0
  #             should_tx = new_curvature == 0
  #
  #           with self.subTest(steer_control_enabled=steer_control_enabled, speed=speed,
  #                             initial_curvature=initial_curvature, new_curvature=new_curvature):
  #             self.assertEqual(should_tx, self._tx(self._tja_command_msg(steer_control_enabled, 0, 0, new_curvature, 0)))

  # def test_prevent_lkas_action(self):
  #   self.safety.set_controls_allowed(1)
  #   self.assertFalse(self._tx(self._lkas_command_msg(1)))
  #
  #   self.safety.set_controls_allowed(0)
  #   self.assertFalse(self._tx(self._lkas_command_msg(1)))
  #
  # def test_acc_buttons(self):
  #   for allowed in (0, 1):
  #     self.safety.set_controls_allowed(allowed)
  #     for enabled in (True, False):
  #       self._rx(self._pcm_status_msg(enabled))
  #       self.assertTrue(self._tx(self._acc_button_msg(Buttons.TJA_TOGGLE, 2)))
  #
  #   for allowed in (0, 1):
  #     self.safety.set_controls_allowed(allowed)
  #     for bus in (0, 2):
  #       self.assertEqual(allowed, self._tx(self._acc_button_msg(Buttons.RESUME, bus)))
  #
  #   for enabled in (True, False):
  #     self._rx(self._pcm_status_msg(enabled))
  #     for bus in (0, 2):
  #       self.assertEqual(enabled, self._tx(self._acc_button_msg(Buttons.CANCEL, bus)))


if __name__ == "__main__":
  unittest.main()
