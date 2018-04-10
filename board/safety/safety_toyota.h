// IPAS override
const int32_t IPAS_OVERRIDE_THRESHOLD = 200;  // disallow controls when user torque exceeds this value

// global torque limit
const int32_t MAX_TORQUE = 1500;       // max torque cmd allowed ever

// rate based torque limit + stay within actually applied
// packet is sent at 100hz, so this limit is 1000/sec
const int32_t MAX_RATE_UP = 10;        // ramp up slow
const int32_t MAX_RATE_DOWN = 25;      // ramp down fast
const int32_t MAX_TORQUE_ERROR = 350;  // max torque cmd in excess of torque motor

struct lookup_t {
  float x[3];
  float y[3];
};

struct sample_t {
  int values[3];
  int min;
  int max;
} sample_t_default = {{0, 0, 0}, 0, 0};

// 2m/s are added to be less restrictive
const struct lookup_t LOOKUP_ANGLE_RATE_UP = {
  {2., 7., 17.},
  {5., .8, .15}};

const struct lookup_t LOOKUP_ANGLE_RATE_DOWN = {
  {2., 7., 17.},
  {5., 3.5, .4}};

const float RT_ANGLE_FUDGE = 1.5;     // for RT checks allow 50% more angle change

// real time torque limit to prevent controls spamming
// the real time limit is 1500/sec
const int32_t MAX_RT_DELTA = 375;      // max delta torque allowed for real time checks
const int32_t RT_INTERVAL = 250000;    // 250ms between real time checks

// longitudinal limits
const int16_t MAX_ACCEL = 1500;        // 1.5 m/s2
const int16_t MIN_ACCEL = -3000;       // 3.0 m/s2

const float CAN_TO_DEG = 2. / 3.;      // convert angles from CAN unit to degrees

int cruise_engaged_last = 0;           // cruise state
int ipas_state = 1;                    // 1 disabled, 3 executing angle control, 5 override
int angle_control = 0;                 // 1 if direct angle control packets are seen
float speed = 0.;

struct sample_t torque_meas;           // last 3 motor torques produced by the eps
struct sample_t angle_meas;            // last 3 steer angles
struct sample_t torque_driver;         // last 3 driver steering torque

// global actuation limit state
int actuation_limits = 1;              // by default steer limits are imposed
int16_t dbc_eps_torque_factor = 100;   // conversion factor for STEER_TORQUE_EPS in %: see dbc file

// state of torque limits
int16_t desired_torque_last = 0;       // last desired steer torque
int16_t desired_angle_last = 0;        // last desired steer angle
int16_t rt_torque_last = 0;            // last desired torque for real time check
int16_t rt_angle_last = 0;             // last desired torque for real time check
uint32_t ts_last = 0;
uint32_t ts_angle_last = 0;

int controls_allowed_last = 0;

int to_signed(int d, int bits) {
  if (d >= (1 << (bits - 1))) {
    d -= (1 << bits);
  }
  return d;
}

// interp function that holds extreme values
float interpolate(struct lookup_t xy, float x) {
  int size = sizeof(xy.x) / sizeof(xy.x[0]);
  // x is lower than the first point in the x array. Return the first point
  if (x <= xy.x[0]) {
    return xy.y[0];

  } else {
    // find the index such that (xy.x[i] <= x < xy.x[i+1]) and linearly interp
    for (int i=0; i < size-1; i++) {
      if (x < xy.x[i+1]) {
        float x0 = xy.x[i];
        float y0 = xy.y[i];
        float dx = xy.x[i+1] - x0;
        float dy = xy.y[i+1] - y0;
        // dx should not be zero as xy.x is supposed ot be monotonic
        if (dx <= 0.) dx = 0.0001;
        return dy * (x - x0) / dx + y0;
      }
    }
    // if no such point is found, then x > xy.x[size-1]. Return last point
    return xy.y[size - 1];
  }
}

uint32_t get_ts_elapsed(uint32_t ts, uint32_t ts_last) {
  return ts > ts_last ? ts - ts_last : (0xFFFFFFFF - ts_last) + 1 + ts;
}

void update_sample(struct sample_t *sample, int sample_new) {
  for (int i = sizeof(sample->values)/sizeof(sample->values[0]) - 1; i > 0; i--) {
    sample->values[i] = sample->values[i-1];
  }
  sample->values[0] = sample_new;

  // get the minimum and maximum measured torque over the last 3 frames
  sample->min = sample->max = sample->values[0];
  for (int i = 1; i < sizeof(sample->values)/sizeof(sample->values[0]); i++) {
    if (sample->values[i] < sample->min) sample->min = sample->values[i];
    if (sample->values[i] > sample->max) sample->max = sample->values[i];
  }
}


static void toyota_rx_hook(CAN_FIFOMailBox_TypeDef *to_push) {

  // EPS torque sensor
  if ((to_push->RIR>>21) == 0x260) {
    // get eps motor torque (see dbc_eps_torque_factor in dbc)
    int16_t torque_meas_new_16 = (((to_push->RDHR) & 0xFF00) | ((to_push->RDHR >> 16) & 0xFF));

    // increase torque_meas by 1 to be conservative on rounding
    int torque_meas_new = ((int)(torque_meas_new_16) * dbc_eps_torque_factor / 100) + (torque_meas_new_16 > 0 ? 1 : -1);

    // update array of sample
    update_sample(&torque_meas, torque_meas_new);

    // get driver steering torque
    int16_t torque_driver_new = (((to_push->RDLR) & 0xFF00) | ((to_push->RDLR >> 16) & 0xFF));

    // update array of samples
    update_sample(&torque_driver, torque_driver_new);
  }

  // get steer angle
  if ((to_push->RIR>>21) == 0x25) {
    int angle_meas_new = ((to_push->RDLR & 0xf) << 8) + ((to_push->RDLR & 0xff00) >> 8);
    uint32_t ts = TIM2->CNT;

    angle_meas_new = to_signed(angle_meas_new, 12);

    // update array of samples
    update_sample(&angle_meas, angle_meas_new);

    // *** angle real time check
    // add 1 to not false trigger the violation and multiply by 25 since the check is done every 250ms
    int rt_delta_angle_up = ((int)(RT_ANGLE_FUDGE * (interpolate(LOOKUP_ANGLE_RATE_UP, speed) * 25. * CAN_TO_DEG + 1.)));
    int rt_delta_angle_down = ((int)(RT_ANGLE_FUDGE * (interpolate(LOOKUP_ANGLE_RATE_DOWN, speed) * 25 * CAN_TO_DEG + 1.)));
    int highest_rt_angle = rt_angle_last + (rt_angle_last > 0? rt_delta_angle_up:rt_delta_angle_down);
    int lowest_rt_angle = rt_angle_last - (rt_angle_last > 0? rt_delta_angle_down:rt_delta_angle_up);

    // every RT_INTERVAL or when controls are turned on, set the new limits
    uint32_t ts_elapsed = get_ts_elapsed(ts, ts_angle_last);
    if ((ts_elapsed > RT_INTERVAL) || (controls_allowed && !controls_allowed_last)) {
      rt_angle_last = angle_meas_new;
      ts_angle_last = ts;
    }

    // check for violation
    if (angle_control &&
        ((angle_meas_new < lowest_rt_angle) ||
         (angle_meas_new > highest_rt_angle))) {
      controls_allowed = 0;
    }

    controls_allowed_last = controls_allowed;
  }

  // get speed
  if ((to_push->RIR>>21) == 0xb4) {
    speed = ((float) (((to_push->RDHR) & 0xFF00) | ((to_push->RDHR >> 16) & 0xFF))) * 0.01 / 3.6;
  }

  // enter controls on rising edge of ACC, exit controls on ACC off
  if ((to_push->RIR>>21) == 0x1D2) {
    // 4 bits: 55-52
    int cruise_engaged = to_push->RDHR & 0xF00000;
    if (cruise_engaged && (!cruise_engaged_last)) {
      controls_allowed = 1;
    } else if (!cruise_engaged) {
      controls_allowed = 0;
    }
    cruise_engaged_last = cruise_engaged;
  }

  // get ipas state
  if ((to_push->RIR>>21) == 0x262) {
    ipas_state = (to_push->RDLR & 0xf);
  }

  // exit controls on high steering override
  if (angle_control && ((torque_driver.min > IPAS_OVERRIDE_THRESHOLD) ||
                        (torque_driver.max < -IPAS_OVERRIDE_THRESHOLD) ||
                        (ipas_state==5))) {
    controls_allowed = 0;
  }
}

static int toyota_tx_hook(CAN_FIFOMailBox_TypeDef *to_send) {

  // Check if msg is sent on BUS 0
  if (((to_send->RDTR >> 4) & 0xF) == 0) {

    // ACCEL: safety check on byte 1-2
    if ((to_send->RIR>>21) == 0x343) {
      int16_t desired_accel = ((to_send->RDLR & 0xFF) << 8) | ((to_send->RDLR >> 8) & 0xFF);
      if (controls_allowed && actuation_limits) {
        if ((desired_accel > MAX_ACCEL) || (desired_accel < MIN_ACCEL)) {
          return 0;
        }
      } else if (!controls_allowed && (desired_accel != 0)) {
        return 0;
      }
    }

    // STEER ANGLE
    if ((to_send->RIR>>21) == 0x266) {

      angle_control = 1;   // we are in angle control mode
      int desired_angle = ((to_send->RDLR & 0xf) << 8) + ((to_send->RDLR & 0xff00) >> 8);
      int ipas_state_cmd = ((to_send->RDLR & 0xff) >> 4);
      int16_t violation = 0;

      desired_angle = to_signed(desired_angle, 12);

      if (controls_allowed) {
        // add 1 to not false trigger the violation
        int delta_angle_up = (int) (interpolate(LOOKUP_ANGLE_RATE_UP, speed) * CAN_TO_DEG + 1.);
        int delta_angle_down = (int) (interpolate(LOOKUP_ANGLE_RATE_DOWN, speed) * CAN_TO_DEG + 1.);
        int highest_desired_angle = desired_angle_last + (desired_angle_last > 0? delta_angle_up:delta_angle_down);
        int lowest_desired_angle = desired_angle_last - (desired_angle_last > 0? delta_angle_down:delta_angle_up);
        if ((desired_angle > highest_desired_angle) || 
            (desired_angle < lowest_desired_angle)){
          violation = 1;
          controls_allowed = 0;
        }
      }
      
      // desired steer angle should be the same as steer angle measured when controls are off
      if ((!controls_allowed) && 
           ((desired_angle < (angle_meas.min - 1)) ||
            (desired_angle > (angle_meas.max + 1)) ||
            (ipas_state_cmd != 1))) {
        violation = 1;
      }

      desired_angle_last = desired_angle;

      if (violation) {
        return false;
      }
    }

    // STEER TORQUE: safety check on bytes 2-3
    if ((to_send->RIR>>21) == 0x2E4) {
      int16_t desired_torque = (to_send->RDLR & 0xFF00) | ((to_send->RDLR >> 16) & 0xFF);
      int16_t violation = 0;

      uint32_t ts = TIM2->CNT;

      // only check if controls are allowed and actuation_limits are imposed
      if (controls_allowed && actuation_limits) {

        // *** global torque limit check ***
        if (desired_torque < -MAX_TORQUE) violation = 1;
        if (desired_torque > MAX_TORQUE) violation = 1;


        // *** torque rate limit check ***
        int16_t highest_allowed_torque = max(desired_torque_last, 0) + MAX_RATE_UP;
        int16_t lowest_allowed_torque = min(desired_torque_last, 0) - MAX_RATE_UP;

        // if we've exceeded the applied torque, we must start moving toward 0
        highest_allowed_torque = min(highest_allowed_torque, max(desired_torque_last - MAX_RATE_DOWN, max(torque_meas.max, 0) + MAX_TORQUE_ERROR));
        lowest_allowed_torque = max(lowest_allowed_torque, min(desired_torque_last + MAX_RATE_DOWN, min(torque_meas.min, 0) - MAX_TORQUE_ERROR));

        // check for violation
        if ((desired_torque < lowest_allowed_torque) || (desired_torque > highest_allowed_torque)) {
          violation = 1;
        }

        // used next time
        desired_torque_last = desired_torque;


        // *** torque real time rate limit check ***
        int16_t highest_rt_torque = max(rt_torque_last, 0) + MAX_RT_DELTA;
        int16_t lowest_rt_torque = min(rt_torque_last, 0) - MAX_RT_DELTA;

        // check for violation
        if ((desired_torque < lowest_rt_torque) || (desired_torque > highest_rt_torque)) {
          violation = 1;
        }

        // every RT_INTERVAL set the new limits
        uint32_t ts_elapsed = get_ts_elapsed(ts, ts_last);
        if (ts_elapsed > RT_INTERVAL) {
          rt_torque_last = desired_torque;
          ts_last = ts;
        }
      }
      
      // no torque if controls is not allowed
      if (!controls_allowed && (desired_torque != 0)) {
        violation = 1;
      }

      // reset to 0 if either controls is not allowed or there's a violation
      if (violation || !controls_allowed) {
        desired_torque_last = 0;
        rt_torque_last = 0;
        ts_last = ts;
      }

      if (violation) {
        return false;
      }
    }
  }

  // 1 allows the message through
  return true;
}

static int toyota_tx_lin_hook(int lin_num, uint8_t *data, int len) {
  // TODO: add safety if using LIN
  return true;
}

static void toyota_init(int16_t param) {
  controls_allowed = 0;
  actuation_limits = 1;
  dbc_eps_torque_factor = param;
}

static int toyota_fwd_hook(int bus_num, CAN_FIFOMailBox_TypeDef *to_fwd) {
  return -1;
}

const safety_hooks toyota_hooks = {
  .init = toyota_init,
  .rx = toyota_rx_hook,
  .tx = toyota_tx_hook,
  .tx_lin = toyota_tx_lin_hook,
  .fwd = toyota_fwd_hook,
};

static void toyota_nolimits_init(int16_t param) {
  controls_allowed = 0;
  actuation_limits = 0;
  dbc_eps_torque_factor = param;
}

const safety_hooks toyota_nolimits_hooks = {
  .init = toyota_nolimits_init,
  .rx = toyota_rx_hook,
  .tx = toyota_tx_hook,
  .tx_lin = toyota_tx_lin_hook,
  .fwd = toyota_fwd_hook,
};
