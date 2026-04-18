#include "uart.h"
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

static void *uart_input_thread(void *arg) {
    uart_t *uart = (uart_t *)arg;
    u8 c;
    while (uart->running) {
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n > 0) {
            pthread_mutex_lock(&uart->lock);
            if (uart->rx_count < 256) {
                uart->rx_fifo[uart->rx_tail] = c;
                uart->rx_tail = (uart->rx_tail + 1) & 0xFF;
                uart->rx_count++;
                /* Set data ready in LSR */
                uart->regs[UART_LSR] |= LSR_DR;
                /* If RX interrupt enabled, signal interrupt */
                if (uart->regs[UART_IER] & IER_RDI) {
                    uart->interrupting = true;
                }
            }
            pthread_mutex_unlock(&uart->lock);
        } else {
            /* Small sleep to avoid busy-waiting */
            usleep(1000);
        }
    }
    return NULL;
}

void uart_init(uart_t *uart) {
    memset(uart->regs, 0, sizeof(uart->regs));
    uart->rx_head = 0;
    uart->rx_tail = 0;
    uart->rx_count = 0;
    uart->interrupting = false;
    uart->thre_ip = true; /* THR is initially empty */
    uart->running = true;

    /* LSR: TX holding register is always empty (ready to send) */
    uart->regs[UART_LSR] = LSR_THRE | LSR_TEMT;

    pthread_mutex_init(&uart->lock, NULL);
    pthread_create(&uart->input_thread, NULL, uart_input_thread, uart);
}

void uart_free(uart_t *uart) {
    uart->running = false;
    pthread_join(uart->input_thread, NULL);
    pthread_mutex_destroy(&uart->lock);
}

u64 uart_load(uart_t *uart, u64 offset, int size) {
    (void)size;
    pthread_mutex_lock(&uart->lock);
    u64 val = 0;

    switch (offset) {
        case UART_RBR:
            /* Read receive buffer */
            if (uart->rx_count > 0) {
                val = uart->rx_fifo[uart->rx_head];
                uart->rx_head = (uart->rx_head + 1) & 0xFF;
                uart->rx_count--;
                if (uart->rx_count == 0) {
                    uart->regs[UART_LSR] &= ~LSR_DR;
                    uart->interrupting = false;
                }
            }
            break;
        case UART_IER:
            val = uart->regs[UART_IER];
            break;
        case UART_IIR: {
            /* Priority: RX (0x04) > TX (0x02) */
            if ((uart->regs[UART_IER] & IER_RDI) && (uart->interrupting)) {
                val = 0x04; /* Received data available */
                uart->interrupting = false; /* Cleared by IIR read */
            } else if ((uart->regs[UART_IER] & IER_THRI) && (uart->thre_ip)) {
                val = 0x02; /* THR empty */
                uart->thre_ip = false; /* Cleared by IIR read */
            } else {
                val = 0x01; /* No interrupt pending */
            }
            break;
        }
        case UART_LCR:
            val = uart->regs[UART_LCR];
            break;
        case UART_MCR:
            val = uart->regs[UART_MCR];
            break;
        case UART_LSR:
            val = uart->regs[UART_LSR];
            break;
        case UART_MSR:
            val = uart->regs[UART_MSR];
            break;
        case UART_SCR:
            val = uart->regs[UART_SCR];
            break;
        default:
            val = 0;
            break;
    }

    pthread_mutex_unlock(&uart->lock);
    return val;
}

void uart_store(uart_t *uart, u64 offset, u64 value, int size) {
    (void)size;
    pthread_mutex_lock(&uart->lock);

    switch (offset) {
        case UART_THR: {
            /* Write to transmit holding register → output to stdout */
            u8 c = (u8)value;
            if (write(STDOUT_FILENO, &c, 1) < 0) { /* ignore */ }
            uart->thre_ip = true; /* Became empty again */
            break;
        }
        case UART_IER:
            uart->regs[UART_IER] = (u8)value;
            break;
        case UART_FCR:
            /* FIFO control — we don't fully emulate FIFOs, just accept writes */
            uart->regs[UART_FCR] = (u8)value;
            break;
        case UART_LCR:
            uart->regs[UART_LCR] = (u8)value;
            break;
        case UART_MCR:
            uart->regs[UART_MCR] = (u8)value;
            break;
        case UART_SCR:
            uart->regs[UART_SCR] = (u8)value;
            break;
        default:
            break;
    }

    pthread_mutex_unlock(&uart->lock);
}

bool uart_is_interrupting(uart_t *uart) {
    pthread_mutex_lock(&uart->lock);
    bool r = false;
    if ((uart->regs[UART_IER] & IER_RDI) && uart->interrupting) r = true;
    if ((uart->regs[UART_IER] & IER_THRI) && uart->thre_ip) r = true;
    pthread_mutex_unlock(&uart->lock);
    return r;
}
