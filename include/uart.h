#ifndef UART_H
#define UART_H

#include "types.h"
#include <pthread.h>

/* NS16550A register offsets */
#define UART_RBR  0  /* Receive Buffer Register (read) */
#define UART_THR  0  /* Transmit Holding Register (write) */
#define UART_IER  1  /* Interrupt Enable Register */
#define UART_IIR  2  /* Interrupt Identification Register (read) */
#define UART_FCR  2  /* FIFO Control Register (write) */
#define UART_LCR  3  /* Line Control Register */
#define UART_MCR  4  /* Modem Control Register */
#define UART_LSR  5  /* Line Status Register */
#define UART_MSR  6  /* Modem Status Register */
#define UART_SCR  7  /* Scratch Register */

/* LSR bits */
#define LSR_DR    (1 << 0)  /* Data Ready */
#define LSR_THRE  (1 << 5)  /* Transmit Holding Register Empty */
#define LSR_TEMT  (1 << 6)  /* Transmitter Empty */

/* IER bits */
#define IER_RDI   (1 << 0)  /* Received Data Available Interrupt */
#define IER_THRI  (1 << 1)  /* Transmitter Holding Register Empty Interrupt */

typedef struct {
    u8 regs[8];
    u8 rx_fifo[256];
    int rx_head;
    int rx_tail;
    int rx_count;
    pthread_mutex_t lock;
    pthread_t input_thread;
    bool running;
    bool interrupting; /* RX interrupting state */
    bool thre_ip;      /* THRE interrupt pending */
} uart_t;

void uart_init(uart_t *uart);
void uart_free(uart_t *uart);
u64  uart_load(uart_t *uart, u64 offset, int size);
void uart_store(uart_t *uart, u64 offset, u64 value, int size);
bool uart_is_interrupting(uart_t *uart);

#endif /* UART_H */
