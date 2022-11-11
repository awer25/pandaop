// /////////////////
// Tres + Harness //
// /////////////////

void tres_init(void) {
  // Enable USB 3.3V LDO for USB block
  register_set_bits(&(PWR->CR3), PWR_CR3_USBREGEN); // Only for tres
  register_set_bits(&(PWR->CR3), PWR_CR3_USB33DEN);
  while ((PWR->CR3 & PWR_CR3_USB33RDY) == 0);

  red_chiplet_init();
}

const board board_tres = {
  .board_type = "Tres",
  .board_tick = unused_board_tick,
  .harness_config = &red_chiplet_harness_config,
  .has_gps = false,
  .has_hw_gmlan = false,
  .has_obd = true,
  .has_lin = false,
  .has_canfd = true,
  .has_rtc_battery = true,
  .fan_max_rpm = 0U,
  .init = tres_init,
  .enable_can_transceiver = red_chiplet_enable_can_transceiver,
  .enable_can_transceivers = red_chiplet_enable_can_transceivers,
  .set_led = red_set_led,
  .set_gps_mode = unused_set_gps_mode,
  .set_can_mode = red_set_can_mode,
  .check_ignition = red_check_ignition,
  .read_current = unused_read_current,
  .set_fan_enabled = unused_set_fan_enabled,
  .set_ir_power = unused_set_ir_power,
  .set_phone_power = unused_set_phone_power,
  .set_clock_source_mode = unused_set_clock_source_mode,
  .set_siren = unused_set_siren
};
