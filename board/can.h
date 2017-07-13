#ifndef PANDA_CAN_H
#define PANDA_CAN_H

#define CAN_TIMEOUT 1000000
#define PANDA_CANB_RETURN_FLAG 0x80

extern int can_live, pending_can_live;

extern CAN_TypeDef *can_numbering[];
extern int8_t can_forwarding[];
extern uint32_t can_bitrate[];

#ifdef PANDA
  #define CAN_MAX 3
#else
  #define CAN_MAX 2
#endif

// ********************* queues types *********************

typedef struct {
  uint32_t w_ptr;
  uint32_t r_ptr;
  uint32_t fifo_size;
  CAN_FIFOMailBox_TypeDef *elems;
} can_ring;

#define can_buffer(x, size) \
  CAN_FIFOMailBox_TypeDef elems_##x[size]; \
  can_ring can_##x = { .w_ptr = 0, .r_ptr = 0, .fifo_size = size, .elems = (CAN_FIFOMailBox_TypeDef *)&elems_##x };

extern can_ring can_rx_q;

// ********************* interrupt safe queue *********************

int pop(can_ring *q, CAN_FIFOMailBox_TypeDef *elem);

int push(can_ring *q, CAN_FIFOMailBox_TypeDef *elem);

// ********************* CAN Functions *********************

void can_init(uint8_t canid);

void send_can(CAN_FIFOMailBox_TypeDef *to_push, int flags);

int can_cksum(uint8_t *dat, int len, int addr, int idx);

extern int controls_allowed;

#endif
