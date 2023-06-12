bool flash_is_locked(void) {
  return (FLASH->CR1 & FLASH_CR_LOCK);
}

void flash_unlock(void) {
  FLASH->KEYR1 = 0x45670123;
  FLASH->KEYR1 = 0xCDEF89AB;
}

void flash_lock(void) {
  FLASH->CR1 |= FLASH_CR_LOCK;
}

bool flash_erase_sector(uint16_t sector) {
  // don't erase the bootloader(sector 0)
  bool ret = false;
  if ((sector != 0U) && (sector < 8U) && (!flash_is_locked())) {
    FLASH->CR1 = (sector << 8) | FLASH_CR_SER;
    FLASH->CR1 |= FLASH_CR_START;
    while ((FLASH->SR1 & FLASH_SR_QW) != 0U);
    ret = true;
  }
  return ret;
}

void flash_write_word(void *prog_ptr, uint32_t data) {
  uint32_t *pp = prog_ptr;
  FLASH->CR1 |= FLASH_CR_PG;
  *pp = data;
  while ((FLASH->SR1 & FLASH_SR_QW) != 0U);
}

void flush_write_buffer(void) {
  if ((FLASH->SR1 & FLASH_SR_WBNE) != 0U) {
    FLASH->CR1 |= FLASH_CR_FW;
    while ((FLASH->SR1 & FLASH_CR_FW) != 0U);
  }
}
