#include "safety_volkswagen_common.h"

// lateral limits
const SteeringLimits VOLKSWAGEN_MQB_STEERING_LIMITS = {
  .max_steer = 300,              // 3.0 Nm (EPS side max of 3.0Nm with fault if violated)
  .max_rt_delta = 75,            // 4 max rate up * 50Hz send rate * 250000 RT interval / 1000000 = 50 ; 50 * 1.5 for safety pad = 75
  .max_rt_interval = 250000,     // 250ms between real time checks
  .max_rate_up = 4,              // 2.0 Nm/s RoC limit (EPS rack has own soft-limit of 5.0 Nm/s)
  .max_rate_down = 10,           // 5.0 Nm/s RoC limit (EPS rack has own soft-limit of 5.0 Nm/s)
  .driver_torque_allowance = 80,
  .driver_torque_factor = 3,
  .type = TorqueDriverLimited,
};

// longitudinal limits
// acceleration in m/s2 * 1000 to avoid floating point math
const LongitudinalLimits VOLKSWAGEN_MQB_LONG_LIMITS = {
  .max_accel = 2000,
  .min_accel = -3500,
  .inactive_accel = 3010,  // VW sends one increment above the max range when inactive
};

// Transmit of GRA_ACC_01 is allowed on bus 0 and 2 to keep compatibility with gateway and camera integration
const CanMsg VOLKSWAGEN_MQB_STOCK_TX_MSGS[] = {{VW_MSG_HCA_01, 0, 8}, {VW_MSG_GRA_ACC_01, 0, 8},
                                               {VW_MSG_GRA_ACC_01, 2, 8}, {VW_MSG_LDW_02, 0, 8}};
const CanMsg VOLKSWAGEN_MQB_LONG_TX_MSGS[] = {{VW_MSG_HCA_01, 0, 8}, {VW_MSG_LDW_02, 0, 8}, {VW_MSG_ACC_02, 0, 8},
                                              {VW_MSG_ACC_06, 0, 8}, {VW_MSG_ACC_07, 0, 8}};

AddrCheckStruct volkswagen_mqb_addr_checks[] = {
  {.msg = {{VW_MSG_ESP_19, 0, 8, .check_checksum = false, .max_counter = 0U, .expected_timestep = 10000U}, { 0 }, { 0 }}},
  {.msg = {{VW_MSG_LH_EPS_03, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 10000U}, { 0 }, { 0 }}},
  {.msg = {{VW_MSG_ESP_05, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}, { 0 }, { 0 }}},
  {.msg = {{VW_MSG_TSK_06, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}, { 0 }, { 0 }}},
  {.msg = {{VW_MSG_MOTOR_20, 0, 8, .check_checksum = true, .max_counter = 15U, .expected_timestep = 20000U}, { 0 }, { 0 }}},
  {.msg = {{VW_MSG_MOTOR_14, 0, 8, .check_checksum = false, .max_counter = 0U, .expected_timestep = 100000U}, { 0 }, { 0 }}},
};
#define VOLKSWAGEN_MQB_ADDR_CHECKS_LEN (sizeof(volkswagen_mqb_addr_checks) / sizeof(volkswagen_mqb_addr_checks[0]))
addr_checks volkswagen_mqb_rx_checks = {volkswagen_mqb_addr_checks, VOLKSWAGEN_MQB_ADDR_CHECKS_LEN};


static const addr_checks* volkswagen_mqb_init(uint16_t param) {
  volkswagen_common_init(param);
  return &volkswagen_mqb_rx_checks;
}

static int volkswagen_mqb_rx_hook(CANPacket_t *to_push) {
  bool valid = addr_safety_check(to_push, &volkswagen_mqb_rx_checks, volkswagen_mqb_get_checksum,
                                 volkswagen_mqb_compute_crc, volkswagen_mqb_get_counter, NULL);

  if (valid && (GET_BUS(to_push) == 0U)) {
    int addr = GET_ADDR(to_push);

    // Update in-motion state by sampling wheel speeds
    if (addr == VW_MSG_ESP_19) {
      // sum 4 wheel speeds
      int speed = 0;
      for (uint8_t i = 0U; i < 8U; i += 2U) {
        int wheel_speed = GET_BYTE(to_push, i) | (GET_BYTE(to_push, i + 1U) << 8);
        speed += wheel_speed;
      }
      // Check all wheel speeds for any movement
      vehicle_moving = speed > 0;
    }

    // Update driver input torque samples
    // Signal: LH_EPS_03.EPS_Lenkmoment (absolute torque)
    // Signal: LH_EPS_03.EPS_VZ_Lenkmoment (direction)
    if (addr == VW_MSG_LH_EPS_03) {
      int torque_driver_new = GET_BYTE(to_push, 5) | ((GET_BYTE(to_push, 6) & 0x1FU) << 8);
      int sign = (GET_BYTE(to_push, 6) & 0x80U) >> 7;
      if (sign == 1) {
        torque_driver_new *= -1;
      }
      update_sample(&torque_driver, torque_driver_new);
    }

    if (addr == VW_MSG_TSK_06) {
      // When using stock ACC, enter controls on rising edge of stock ACC engage, exit on disengage
      // Always exit controls on main switch off
      // Signal: TSK_06.TSK_Status
      int acc_status = (GET_BYTE(to_push, 3) & 0x7U);
      bool cruise_engaged = (acc_status == 3) || (acc_status == 4) || (acc_status == 5);
      acc_main_on = cruise_engaged || (acc_status == 2);

      if (!volkswagen_longitudinal) {
        pcm_cruise_check(cruise_engaged);
      }

      if (!acc_main_on) {
        controls_allowed = false;
      }
    }

    if (addr == VW_MSG_GRA_ACC_01) {
      // If using openpilot longitudinal, enter controls on falling edge of Set or Resume with main switch on
      // Signal: GRA_ACC_01.GRA_Tip_Setzen
      // Signal: GRA_ACC_01.GRA_Tip_Wiederaufnahme
      if (volkswagen_longitudinal) {
        bool set_button = GET_BIT(to_push, 16U);
        bool resume_button = GET_BIT(to_push, 19U);
        if ((volkswagen_set_button_prev && !set_button) || (volkswagen_resume_button_prev && !resume_button)) {
          controls_allowed = acc_main_on;
        }
        volkswagen_set_button_prev = set_button;
        volkswagen_resume_button_prev = resume_button;
      }
      // Always exit controls on rising edge of Cancel
      // Signal: GRA_ACC_01.GRA_Abbrechen
      if (GET_BIT(to_push, 13U) == 1U) {
        controls_allowed = false;
      }
    }

    if (addr == VW_MSG_MOTOR_20) {
      // Signal: Motor_20.MO_Fahrpedalrohwert_01
      gas_pressed = ((GET_BYTES(to_push, 0, 4) >> 12) & 0xFFU) != 0U;
    }

    if (addr == VW_MSG_MOTOR_14) {
      // Signal: Motor_14.MO_Fahrer_bremst (ECU detected brake pedal switch F63)
      volkswagen_brake_pedal_switch = GET_BIT(to_push, 28U);
    }

    if (addr == VW_MSG_ESP_05) {
      // Signal: ESP_05.ESP_Fahrer_bremst (ESP detected driver brake pressure above platform specified threshold)
      volkswagen_brake_pressure_detected = GET_BIT(to_push, 26U);
    }

    brake_pressed = volkswagen_brake_pedal_switch || volkswagen_brake_pressure_detected;
    generic_rx_checks((addr == VW_MSG_HCA_01));
  }

  return valid;
}

static int volkswagen_mqb_tx_hook(CANPacket_t *to_send) {
  int addr = GET_ADDR(to_send);
  int tx = 1;

  if (volkswagen_longitudinal) {
    tx = msg_allowed(to_send, VOLKSWAGEN_MQB_LONG_TX_MSGS, sizeof(VOLKSWAGEN_MQB_LONG_TX_MSGS) / sizeof(VOLKSWAGEN_MQB_LONG_TX_MSGS[0]));
  } else {
    tx = msg_allowed(to_send, VOLKSWAGEN_MQB_STOCK_TX_MSGS, sizeof(VOLKSWAGEN_MQB_STOCK_TX_MSGS) / sizeof(VOLKSWAGEN_MQB_STOCK_TX_MSGS[0]));
  }

  // Safety check for HCA_01 Heading Control Assist torque
  // Signal: HCA_01.HCA_01_LM_Offset (absolute torque)
  // Signal: HCA_01.HCA_01_LM_OffSign (direction)
  if (addr == VW_MSG_HCA_01) {
    int desired_torque = GET_BYTE(to_send, 2) | ((GET_BYTE(to_send, 3) & 0x1U) << 8);
    bool sign = GET_BIT(to_send, 31U);
    if (sign) {
      desired_torque *= -1;
    }

    bool steer_req = GET_BIT(to_send, 30U) != 0U;

    if (steer_torque_cmd_checks(desired_torque, steer_req, VOLKSWAGEN_MQB_STEERING_LIMITS)) {
      tx = 0;
    }
  }

  // Safety check for both ACC_06 and ACC_07 acceleration requests
  // To avoid floating point math, scale upward and compare to pre-scaled safety m/s2 boundaries
  if ((addr == VW_MSG_ACC_06) || (addr == VW_MSG_ACC_07)) {
    bool violation = false;
    int desired_accel = 0;

    if (addr == VW_MSG_ACC_06) {
      // Signal: ACC_06.ACC_Sollbeschleunigung_02 (acceleration in m/s2, scale 0.005, offset -7.22)
      desired_accel = ((((GET_BYTE(to_send, 4) & 0x7U) << 8) | GET_BYTE(to_send, 3)) * 5U) - 7220U;
    } else {
      // Signal: ACC_07.ACC_Folgebeschl (acceleration in m/s2, scale 0.03, offset -4.6)
      int secondary_accel = (GET_BYTE(to_send, 4) * 30U) - 4600U;
      violation |= (secondary_accel != 3020);  // enforce always inactive (one increment above max range) at this time
      // Signal: ACC_07.ACC_Sollbeschleunigung_02 (acceleration in m/s2, scale 0.005, offset -7.22)
      desired_accel = (((GET_BYTE(to_send, 7) << 3) | ((GET_BYTE(to_send, 6) & 0xE0U) >> 5)) * 5U) - 7220U;
    }

    violation |= longitudinal_accel_checks(desired_accel, VOLKSWAGEN_MQB_LONG_LIMITS);

    if (violation) {
      tx = 0;
    }
  }

  if (addr == VW_MSG_GRA_ACC_01) {
    // Disallow resume and set while controls are disabled
    // Signal: GRA_ACC_01.GRA_Tip_Setzen
    // Signal: GRA_ACC_01.GRA_Tip_Wiederaufnahme
    if (!controls_allowed && (GET_BIT(to_send, 16) || GET_BIT(to_send, 19))) {
      tx = 0;
    }
  }

  return tx;
}

static int volkswagen_mqb_fwd_hook(int bus_num, int addr) {
  int bus_fwd = -1;

  switch (bus_num) {
    case 0:
      // Forward all traffic from the Extended CAN onward
      bus_fwd = 2;
      break;
    case 2:
      if ((addr == VW_MSG_HCA_01) || (addr == VW_MSG_LDW_02)) {
        // openpilot takes over LKAS steering control and related HUD messages from the camera
        bus_fwd = -1;
      } else if (volkswagen_longitudinal && ((addr == VW_MSG_ACC_02) || (addr == VW_MSG_ACC_06) || (addr == VW_MSG_ACC_07))) {
        // openpilot takes over acceleration/braking control and related HUD messages from the stock ACC radar
        bus_fwd = -1;
      } else {
        // Forward all remaining traffic from Extended CAN devices to J533 gateway
        bus_fwd = 0;
      }
      break;
    default:
      // No other buses should be in use; fallback to do-not-forward
      bus_fwd = -1;
      break;
  }

  return bus_fwd;
}

const safety_hooks volkswagen_mqb_hooks = {
  .init = volkswagen_mqb_init,
  .rx = volkswagen_mqb_rx_hook,
  .tx = volkswagen_mqb_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = volkswagen_mqb_fwd_hook,
};
