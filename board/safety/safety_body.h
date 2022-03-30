

static int body_tx_hook(CANPacket_t *to_send) {

  int tx = 0;
  int addr = GET_ADDR(to_send);

  if (addr == 0x200) {
    tx = 1;
  }

  // 1 allows the message through
  return tx;
}

const safety_hooks body_hooks = {
  .init = nooutput_init,
  .rx = default_rx_hook,
  .tx = body_tx_hook,
  .tx_lin = nooutput_tx_lin_hook,
  .fwd = default_fwd_hook,
};
