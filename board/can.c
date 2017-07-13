#include <stdint.h>
#include "config.h"
#include "can.h"
#include "uart.h"
#include "gpio.h"
#include "libc.h"
#include "safety.h"

int can_live = 0, pending_can_live = 0;

// assign CAN numbering
#ifdef PANDA
  can_buffer(rx_q, 0x1000)
  can_buffer(tx1_q, 0x100)
  can_buffer(tx2_q, 0x100)
  can_buffer(tx3_q, 0x100)
  // panda:       CAN1 = 0   CAN2 = 1   CAN3 = 2
  CAN_TypeDef *can_numbering[] = {CAN1, CAN2, CAN3};
  int8_t can_forwarding[] = {-1, -1, -1};
  uint32_t can_bitrate[] = {CAN_DEFAULT_BITRATE,
			  CAN_DEFAULT_BITRATE,
			  CAN_DEFAULT_BITRATE};
#else
  can_buffer(rx_q, 0x1000)
  can_buffer(tx1_q, 0x100)
  can_buffer(tx2_q, 0x100)
  // old:         CAN1 = 1   CAN2 = 0
  CAN_TypeDef *can_numbering[] = {CAN2, CAN1};
  int8_t can_forwarding[] = {-1,-1};
  uint32_t can_bitrate[] = {CAN_DEFAULT_BITRATE,
			    CAN_DEFAULT_BITRATE};
#endif

int controls_allowed = 0;

int pop(can_ring *q, CAN_FIFOMailBox_TypeDef *elem) {
  if (q->w_ptr != q->r_ptr) {
    *elem = q->elems[q->r_ptr];
    if ((q->r_ptr + 1) == q->fifo_size) q->r_ptr = 0;
    else q->r_ptr += 1;
    return 1;
  }
  return 0;
}

int push(can_ring *q, CAN_FIFOMailBox_TypeDef *elem) {
  uint32_t next_w_ptr;
  if ((q->w_ptr + 1) == q->fifo_size) next_w_ptr = 0;
  else next_w_ptr = q->w_ptr + 1;
  if (next_w_ptr != q->r_ptr) {
    q->elems[q->w_ptr] = *elem;
    q->w_ptr = next_w_ptr;
    return 1;
  }
  puts("push failed!\n");
  return 0;
}

// ********************* CAN Functions *********************

void can_init(uint8_t canid) {
  uint32_t bitrate = can_bitrate[canid];
  CAN_TypeDef *CAN = can_numbering[canid];
  uint8_t quanta;
  uint16_t prescaler;
  uint8_t seq1, seq2;

  puts("Configuring Can Interface index ");
  puth(canid);
  puts("\n");

  //MAX 1 Megabaud
  if(bitrate > 1000000)
    bitrate = 1000000;

  #ifdef DEBUG
  puts("Can Speed request to ");
  puth(bitrate);
  puts("\n");
  #endif

  if(canid >= CAN_MAX) return;

  bitrate = can_bitrate[canid];
  CAN = can_numbering[canid];

  //TODO: Try doing both and find the more accurate values.
  if(min((FREQ / 2) / bitrate, 16) == 16){
    quanta = 16;
    seq1 = 13;//roundf(quanta * 0.875f) - 1;
    seq2 = 2;//roundf(quanta * 0.125f);
  }else{
    quanta = 8;
    seq1 = 6;//roundf(quanta * 0.875f) - 1;
    seq2 = 1;//roundf(quanta * 0.125f);
  }

  // TODO: Look into better accuracy with rounding.
  prescaler = FREQ / quanta / bitrate;

  //Check the prescaler is not larger than max
  if(prescaler > 0x3FF)
    prescaler = 0x3FF;

  can_bitrate[canid] = FREQ/quanta/prescaler;

  #ifdef DEBUG
  puts("Can Speed set to ");
  puth(can_bitrate[canid]);
  puts("\n");
  #endif

  set_can_enable(CAN, 1);

  // Move CAN to initialization mode and sync.
  CAN->MCR = CAN_MCR_TTCM | CAN_MCR_INRQ;
  while((CAN->MSR & CAN_MSR_INAK) != CAN_MSR_INAK);

  // seg 1: 13 time quanta, seg 2: 2 time quanta
  CAN->BTR = (CAN_BTR_TS1_0 * (seq1 - 1)) |
             (CAN_BTR_TS2_0 * (seq2 - 1)) |
             (prescaler - 1);

  // silent loopback mode for debugging
  #ifdef CAN_LOOPBACK_MODE
    CAN->BTR |= CAN_BTR_SILM | CAN_BTR_LBKM;
  #endif

  if (!controls_allowed) {
    CAN->BTR |= CAN_BTR_SILM;
  }

  // reset
  CAN->MCR = CAN_MCR_TTCM;

  int tmp = 0;
  while((CAN->MSR & CAN_MSR_INAK) == CAN_MSR_INAK && tmp < CAN_TIMEOUT) tmp++;

  if (tmp == CAN_TIMEOUT) {
    set_led(LED_BLUE, 1);
    puts("CAN init FAILED!!!!!\n");
  } else {
    puts("CAN init done\n");
  }

  // accept all filter
  CAN->FMR |= CAN_FMR_FINIT;

  // no mask
  CAN->sFilterRegister[0].FR1 = 0;
  CAN->sFilterRegister[0].FR2 = 0;
  CAN->sFilterRegister[14].FR1 = 0;
  CAN->sFilterRegister[14].FR2 = 0;
  CAN->FA1R |= 1 | (1 << 14);

  CAN->FMR &= ~(CAN_FMR_FINIT);

  // enable all CAN interrupts
  CAN->IER = 0xFFFFFFFF;
  //CAN->IER = CAN_IER_TMEIE | CAN_IER_FMPIE0 | CAN_IER_FMPIE1;
}

// CAN error
void can_sce(CAN_TypeDef *CAN) {
  #ifdef DEBUG
    if (CAN==CAN1) puts("CAN1:  ");
    if (CAN==CAN2) puts("CAN2:  ");
    #ifdef CAN3
      if (CAN==CAN3) puts("CAN3:  ");
    #endif
    puts("MSR:");
    puth(CAN->MSR);
    puts(" TSR:");
    puth(CAN->TSR);
    puts(" RF0R:");
    puth(CAN->RF0R);
    puts(" RF1R:");
    puth(CAN->RF1R);
    puts(" ESR:");
    puth(CAN->ESR);
    puts("\n");
  #endif

  // clear
  //CAN->sTxMailBox[0].TIR &= ~(CAN_TI0R_TXRQ);
  CAN->TSR |= CAN_TSR_ABRQ0;
  //CAN->ESR |= CAN_ESR_LEC;
  //CAN->MSR &= ~(CAN_MSR_ERRI);
  CAN->MSR = CAN->MSR;
}

void CAN1_SCE_IRQHandler() {
  //puts("CAN1_SCE\n");
  can_sce(CAN1);
}

void CAN2_SCE_IRQHandler() {
  //puts("CAN2_SCE\n");
  can_sce(CAN2);
}

#ifdef CAN3
void CAN3_SCE_IRQHandler() {
  //puts("CAN3_SCE\n");
  can_sce(CAN3);
}
#endif

// CAN receive handlers
// blink blue when we are receiving CAN messages
void can_rx(CAN_TypeDef *CAN, int can_index) {
  //int can_number = can_numbering[can_index];
  while (CAN->RF0R & CAN_RF0R_FMP0) {
    // can is live
    pending_can_live = 1;

    // add to my fifo
    CAN_FIFOMailBox_TypeDef to_push;
    to_push.RIR = CAN->sFIFOMailBox[0].RIR;
    to_push.RDTR = CAN->sFIFOMailBox[0].RDTR;
    to_push.RDLR = CAN->sFIFOMailBox[0].RDLR;
    to_push.RDHR = CAN->sFIFOMailBox[0].RDHR;

    // forwarding (panda only)
    #ifdef PANDA
      if (can_forwarding[can_index] != -1 && can_forwarding[can_index] != -1) {
        CAN_FIFOMailBox_TypeDef to_send;
        to_send.RIR = to_push.RIR | 1; // TXRQ
        to_send.RDTR = to_push.RDTR;
        to_send.RDLR = to_push.RDLR;
        to_send.RDHR = to_push.RDHR;
        send_can(&to_send, can_forwarding[can_index]);
      }
    #endif

    // modify RDTR for our API
    to_push.RDTR = (to_push.RDTR & 0xFFFF000F) | (can_index << 4);

    safety_rx_hook(&to_push);

    #ifdef PANDA
      set_led(LED_GREEN, 1);
    #endif
    push(&can_rx_q, &to_push);

    // next
    CAN->RF0R |= CAN_RF0R_RFOM0;
  }
}

void CAN1_RX0_IRQHandler() {
  //puts("CANRX1");
  //delay(10000);
  can_rx(CAN1, 0);
}

void CAN2_RX0_IRQHandler() {
  //puts("CANRX0");
  //delay(10000);
  can_rx(CAN2, 1);
}

#ifdef CAN3
void CAN3_RX0_IRQHandler() {
  //puts("CANRX0");
  //delay(10000);
  can_rx(CAN3, 2);
}
#endif

int can_cksum(uint8_t *dat, int len, int addr, int idx) {
  int i;
  int s = 0;
  for (i = 0; i < len; i++) {
    s += (dat[i] >> 4);
    s += dat[i] & 0xF;
  }
  s += (addr>>0)&0xF;
  s += (addr>>4)&0xF;
  s += (addr>>8)&0xF;
  s += idx;
  s = 8-s;
  return s&0xF;
}

void process_can(CAN_TypeDef *CAN, can_ring *can_q, int can_number) {
  #ifdef DEBUG
    puts("process CAN TX\n");
  #endif

  // add successfully transmitted message to my fifo
  if ((CAN->TSR & CAN_TSR_TXOK0) == CAN_TSR_TXOK0) {
    CAN_FIFOMailBox_TypeDef to_push;
    to_push.RIR = CAN->sTxMailBox[0].TIR;
    to_push.RDTR = (CAN->sTxMailBox[0].TDTR & 0xFFFF000F) |
      ((PANDA_CANB_RETURN_FLAG | (can_number & 0x7F)) << 4);
    puts("RDTR: ");
    puth(to_push.RDTR);
    puts("\n");
    to_push.RDLR = CAN->sTxMailBox[0].TDLR;
    to_push.RDHR = CAN->sTxMailBox[0].TDHR;
    push(&can_rx_q, &to_push);
  }

  // check for empty mailbox
  CAN_FIFOMailBox_TypeDef to_send;
  if ((CAN->TSR & CAN_TSR_TME0) == CAN_TSR_TME0) {
    if (pop(can_q, &to_send)) {
      // only send if we have received a packet
      CAN->sTxMailBox[0].TDLR = to_send.RDLR;
      CAN->sTxMailBox[0].TDHR = to_send.RDHR;
      CAN->sTxMailBox[0].TDTR = to_send.RDTR;
      CAN->sTxMailBox[0].TIR = to_send.RIR;
    }
  }

  // clear interrupt
  CAN->TSR |= CAN_TSR_RQCP0;
}

// send more, possible for these to not trigger?


void CAN1_TX_IRQHandler() {
  process_can(can_numbering[0], &can_tx1_q, 0);
}

void CAN2_TX_IRQHandler() {
  process_can(can_numbering[1], &can_tx2_q, 1);
}

#ifdef PANDA
void CAN3_TX_IRQHandler() {
  process_can(can_numbering[2], &can_tx3_q, 2);
}
#endif

void send_can(CAN_FIFOMailBox_TypeDef *to_push, int flags) {
  can_ring *can_q;
  CAN_TypeDef *CAN = can_numbering[flags];
  switch(flags){
  case 0:
    can_q = &can_tx1_q;
    break;
  case 1:
    can_q = &can_tx2_q;
    break;
  #ifdef CAN3
  case 2:
    can_q = &can_tx3_q;
    break;
  #endif
  default:
    // no crash
    return;
  }

  // add CAN packet to send queue
  // bus number isn't passed through
  to_push->RDTR &= 0xF;
  push(can_q, to_push);

  // flags = can_number
  process_can(CAN, can_q, flags);
}
