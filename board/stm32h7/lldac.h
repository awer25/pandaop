

void dac_init(DAC_TypeDef *DAC, uint8_t channel, bool dma) {
  register_set(&DAC->CR, 0U, 0xFFFFU);
  register_set(&DAC->MCR, 0U, 0xFFFFU);

  switch(channel) {
    case 1:
      if (dma) {
        register_set_bits(&DAC->CR, DAC_CR_DMAEN1);
        // register_set(&DAC->CR, (6U << DAC_CR_TSEL1_Pos), DAC_CR_TSEL1);
        register_set_bits(&DAC->CR, DAC_CR_TEN1);
      } else {
        register_clear_bits(&DAC->CR, DAC_CR_DMAEN1);
      }
      register_set_bits(&DAC->CR, DAC_CR_EN1);
      break;
    case 2:    
      if (dma) {
        register_set_bits(&DAC->CR, DAC_CR_DMAEN2);
      } else {
        register_clear_bits(&DAC->CR, DAC_CR_DMAEN2);
      }
      register_set_bits(&DAC->CR, DAC_CR_EN2);
      break;
    default:
      break;
  }
}

// Set channel 1 value, in mV
void dac_set(DAC_TypeDef *DAC, uint8_t channel, uint32_t value) {
  uint32_t raw_val = MAX(MIN(value * (1U << 8U) / 3300U, (1U << 8U)), 0U);  
  switch(channel) {
    case 1:
      register_set(&DAC->DHR8R1, raw_val, 0xFFU);
      break;
    case 2:
      register_set(&DAC->DHR8R2, raw_val, 0xFFU);
      break;
    default:
      break;
  }
}
