bool bootkick_ign_prev = false;
BootState boot_state = BOOT_BOOTKICK;
uint8_t bootkick_harness_status_prev = HARNESS_STATUS_NC;

uint8_t boot_reset_countdown = 0;
uint8_t waiting_to_boot_countdown = 0;
bool bootkick_reset_triggered = false;

void bootkick_tick(bool ignition, bool recent_heartbeat) {
  BootState boot_state_prev = boot_state;
  const bool harness_inserted = (harness.status != bootkick_harness_status_prev) && (harness.status != HARNESS_STATUS_NC);

  if ((ignition && !bootkick_ign_prev) || harness_inserted) {
    // bootkick on rising edge of ignition or harness insertion
    boot_state = BOOT_BOOTKICK;
  } else if (recent_heartbeat) {
    // disable bootkick once openpilot is up
    boot_state = BOOT_STANDBY;
  } else {

  }

  // ensure SOM boots
  if ((boot_state == BOOT_BOOTKICK) && (boot_state_prev == BOOT_STANDBY)) {
    waiting_to_boot_countdown = 45U;
  }
  if (waiting_to_boot_countdown > 0U) {
    if (current_board->read_som_gpio() || (boot_state != BOOT_BOOTKICK)) {
      waiting_to_boot_countdown = 0U;
    } else {
      waiting_to_boot_countdown -= 1U;

      // try a reset
      if (waiting_to_boot_countdown == 0U) {
        boot_reset_countdown = 5U;
      }
    }
  }

  if (boot_reset_countdown > 0U) {
    boot_reset_countdown--;
    boot_state = BOOT_RESET;
    bootkick_reset_triggered = true;
  } else if (boot_state == BOOT_RESET) {
    boot_state = BOOT_BOOTKICK;
  } else {

  }

  // update state
  bootkick_ign_prev = ignition;
  bootkick_harness_status_prev = harness.status;
  current_board->set_bootkick(boot_state);
}
