
void dma_pointer_handler(uart_ring *q, uint32_t dma_ndtr) { UNUSED(q); UNUSED(dma_ndtr); }
void uart_rx_ring(uart_ring *q) { UNUSED(q); }
void uart_tx_ring(uart_ring *q) { UNUSED(q); }
void dma_rx_init(uart_ring *q) { UNUSED(q); }

#define __DIV(_PCLK_, _BAUD_)                    (((_PCLK_) * 25U) / (4U * (_BAUD_)))
#define __DIVMANT(_PCLK_, _BAUD_)                (__DIV((_PCLK_), (_BAUD_)) / 100U)
#define __DIVFRAQ(_PCLK_, _BAUD_)                ((((__DIV((_PCLK_), (_BAUD_)) - (__DIVMANT((_PCLK_), (_BAUD_)) * 100U)) * 16U) + 50U) / 100U)
#define __USART_BRR(_PCLK_, _BAUD_)              ((__DIVMANT((_PCLK_), (_BAUD_)) << 4) | (__DIVFRAQ((_PCLK_), (_BAUD_)) & 0x0FU))

void uart_set_baud(USART_TypeDef *u, unsigned int baud) {
  // UART7 is connected to APB1 at 60MHz
  u->BRR = __USART_BRR(60000000U, baud);
}

// This read after reading ISR clears all error interrupts. We don't want compiler warnings, nor optimizations
#define UART_READ_RDR(uart) volatile uint8_t t = (uart)->RDR; UNUSED(t);

void uart_interrupt_handler(uart_ring *q) {
  ENTER_CRITICAL();

  // Read UART status. This is also the first step necessary in clearing most interrupts
  uint32_t status = q->uart->ISR;

  // If RXFNE is set, perform a read. This clears RXFNE, ORE, IDLE, NF and FE
  if((status & USART_ISR_RXNE_RXFNE) != 0U){
    uart_rx_ring(q);
  }

  // Detect errors and clear them
  uint32_t err = (status & USART_ISR_ORE) | (status & USART_ISR_NE) | (status & USART_ISR_FE) | (status & USART_ISR_PE);
  if(err != 0U){
    #ifdef DEBUG_UART
      puts("Encountered UART error: "); puth(err); puts("\n");
    #endif
    UART_READ_RDR(q->uart)
  }
  // Send if necessary
  uart_tx_ring(q);

  // Run DMA pointer handler if the line is idle
  if(q->dma_rx && (status & USART_ISR_IDLE)){
    // Reset IDLE flag
    UART_READ_RDR(q->uart)

    #ifdef DEBUG_UART
      puts("No IDLE dma_pointer_handler implemented for this UART.");
    #endif
  }

  EXIT_CRITICAL();
}

void UART7_IRQ_Handler(void) { uart_interrupt_handler(&uart_ring_som_debug); }

void uart_init(uart_ring *q, int baud) {
  if (q->uart == UART7) {
    REGISTER_INTERRUPT(UART7_IRQn, UART7_IRQ_Handler, 150000U, FAULT_INTERRUPT_RATE_UART_7)
  }

  if (q->dma_rx) {
    // TODO
  }

  uart_set_baud(q->uart, baud);
  q->uart->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
  if (q->uart == UART7) {
    // TODO: what does this do?
    q->uart->CR1 |= USART_CR1_RXNEIE;
  }

  // Enable UART interrupts
  if (q->uart == UART7) {
    NVIC_EnableIRQ(UART7_IRQn);
  }

  // Initialise RX DMA if used
  if (q->dma_rx) {
    dma_rx_init(q);
  }
}
