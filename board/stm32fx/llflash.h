bool flash_is_locked(void) {
  return (FLASH->CR & FLASH_CR_LOCK);
}

void flash_unlock(void) {
  FLASH->KEYR = 0x45670123;
  FLASH->KEYR = 0xCDEF89AB;
}

void flash_lock(void) {
  FLASH->CR |= FLASH_CR_LOCK;
}

bool flash_erase_sector(uint16_t sector) {
  // don't erase the bootloader(sector 0)
  bool ret = false;
  if ((sector != 0U) && (sector < 12U) && (!flash_is_locked())) {
    FLASH->CR = (sector << 3) | FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_STRT;
    while ((FLASH->SR & FLASH_SR_BSY) != 0U);
    ret = true;
  }
  return ret;
}

void flash_write_word(void *prog_ptr, uint32_t data) {
  uint32_t *pp = prog_ptr;
  FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
  *pp = data;
  while ((FLASH->SR & FLASH_SR_BSY) != 0U);
}

void flush_write_buffer(void) { }
