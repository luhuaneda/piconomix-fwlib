/* =============================================================================
     ____    ___    ____    ___    _   _    ___    __  __   ___  __  __ TM
    |  _ \  |_ _|  / ___|  / _ \  | \ | |  / _ \  |  \/  | |_ _| \ \/ /
    | |_) |  | |  | |     | | | | |  \| | | | | | | |\/| |  | |   \  /
    |  __/   | |  | |___  | |_| | | |\  | | |_| | | |  | |  | |   /  \
    |_|     |___|  \____|  \___/  |_| \_|  \___/  |_|  |_| |___| /_/\_\

    Copyright (c) 2018 Pieter Conradie <https://piconomix.com>

    License: MIT
    https://github.com/piconomix/piconomix-fwlib/blob/master/LICENSE.md

    Title:          px_uart.h : UART peripheral driver
    Author(s):      Pieter Conradie
    Creation Date:  2018-02-01

============================================================================= */

/* _____STANDARD INCLUDES____________________________________________________ */
#include <stdlib.h>

/* _____PROJECT INCLUDES_____________________________________________________ */
#include "px_uart.h"
#include "px_board.h"
#include "px_ring_buf.h"
#include "px_lib_stm32cube.h"
#include "px_dbg.h"

/* _____LOCAL DEFINITIONS____________________________________________________ */
PX_DBG_DECL_NAME("uart");

/// Internal data for each SPI peripheral
typedef struct px_uart_per_s
{
    USART_TypeDef * usart_base_adr;     /// USART peripheral base register address
    volatile bool   tx_done;            /// Transmit done flag
    px_ring_buf_t   tx_circ_buf;        /// Transmit circular buffer
    px_ring_buf_t   rx_circ_buf;        /// Receive circular buffer
    px_uart_nr_t    uart_nr;            /// Peripheral
    uint8_t         open_counter;       /// Number of open handles referencing peripheral
} px_uart_per_t;

/* _____MACROS_______________________________________________________________ */

/* _____GLOBAL VARIABLES_____________________________________________________ */

/* _____LOCAL VARIABLES______________________________________________________ */
/// Allocate data for each enabled UART peripheral
#if PX_UART_CFG_UART1_EN
static uint8_t        px_uart1_tx_circ_buf_data[PX_UART_CFG_UART1_TX_BUF_SIZE];
static uint8_t        px_uart1_rx_circ_buf_data[PX_UART_CFG_UART1_RX_BUF_SIZE];
static px_uart_per_t px_uart_per_1;
#endif

#if PX_UART_CFG_UART2_EN
static uint8_t        px_uart2_tx_circ_buf_data[PX_UART_CFG_UART2_TX_BUF_SIZE];
static uint8_t        px_uart2_rx_circ_buf_data[PX_UART_CFG_UART2_RX_BUF_SIZE];
static px_uart_per_t px_uart_per_2;
#endif

#if PX_UART_CFG_UART3_EN
static uint8_t        px_uart3_tx_circ_buf_data[PX_UART_CFG_UART3_TX_BUF_SIZE];
static uint8_t        px_uart3_rx_circ_buf_data[PX_UART_CFG_UART3_RX_BUF_SIZE];
static px_uart_per_t px_uart_per_3;
#endif

#if PX_UART_CFG_UART4_EN
static uint8_t        px_uart4_tx_circ_buf_data[PX_UART_CFG_UART4_TX_BUF_SIZE];
static uint8_t        px_uart4_rx_circ_buf_data[PX_UART_CFG_UART4_RX_BUF_SIZE];
static px_uart_per_t px_uart_per_4;
#endif

#if PX_UART_CFG_UART5_EN
static uint8_t        px_uart5_tx_circ_buf_data[PX_UART_CFG_UART5_TX_BUF_SIZE];
static uint8_t        px_uart5_rx_circ_buf_data[PX_UART_CFG_UART5_RX_BUF_SIZE];
static px_uart_per_t px_uart_per_5;
#endif

/* _____LOCAL FUNCTIONS______________________________________________________ */
/// Generic interrupt handler
static void uart_irq_handler(px_uart_per_t * uart_per)
{
    USART_TypeDef * usart_base_adr = uart_per->usart_base_adr;
    uint8_t         data;

    // Received a byte?
    if(LL_USART_IsActiveFlag_RXNE(usart_base_adr))
    {
        // Read receive data register
        data = LL_USART_ReceiveData8(usart_base_adr);

        // Overrrun Error?
        if(LL_USART_IsActiveFlag_ORE(usart_base_adr))
        {
            // Clear error flag
            LL_USART_ClearFlag_ORE(usart_base_adr);
        }
        // Parity Error?
        if(LL_USART_IsActiveFlag_PE(usart_base_adr))
        {
            // Clear error flag
            LL_USART_ClearFlag_PE(usart_base_adr);
        }
        // Framing Error?
        else if(LL_USART_IsActiveFlag_FE(usart_base_adr))
        {
            // Clear error flag
            LL_USART_ClearFlag_FE(usart_base_adr);
        }
        else
        {
            // Add received byte to circular buffer
            // (byte is discarded if buffer is full)
            px_ring_buf_wr_u8(&uart_per->rx_circ_buf, data);
        }
    }

    // Transmit data register interrupt enabled?
    if(LL_USART_IsEnabledIT_TXE(usart_base_adr))
    {
        // Transmit data register empty?
        if(LL_USART_IsActiveFlag_TXE(usart_base_adr))
        {
            // Data to transmit?
            if(px_ring_buf_rd_u8(&uart_per->tx_circ_buf, &data))
            {
                // Load transmit register with data
                LL_USART_TransmitData8(usart_base_adr, data);
                // Clear flag to indicate that transmission is busy
                uart_per->tx_done = false;
            }
            else
            {
                // Disable Transmit data register empty interrupt
                LL_USART_DisableIT_TXE(usart_base_adr);
                // Enable Transmit complete interrupt
                LL_USART_EnableIT_TC(usart_base_adr);
            }
        }
    }

    // Transmit complete interrupt enabled?
    if(LL_USART_IsEnabledIT_TC(usart_base_adr))
    {
        // Transmit complete?
        if(LL_USART_IsActiveFlag_TC(usart_base_adr))
        {
            // Set flag to indicate that transmission has finished
            uart_per->tx_done = true;
            // Disable Transmit complete interrupt
            LL_USART_DisableIT_TC(usart_base_adr);
        }
    }
}

#if PX_UART_CFG_UART1_EN
/// USART1 interrupt handler
void USART1_IRQHandler(void)
{
    uart_irq_handler(&px_uart_per_1);
}
#endif

#if PX_UART_CFG_UART2_EN
/// USART2 interrupt handler
void USART2_IRQHandler(void)
{
    uart_irq_handler(&px_uart_per_2);
}
#endif

#if PX_UART_CFG_UART3_EN
/// USART2 interrupt handler
void USART3_IRQHandler(void)
{
    uart_irq_handler(&px_uart_per_3);
}
#endif

#if PX_UART_CFG_UART4_EN || PX_UART_CFG_UART5_EN
/// USART4 & USART5 interrupt handler
void USART4_5_IRQHandler(void)
{
#if PX_UART_CFG_UART4_EN
    uart_irq_handler(&px_uart_per_4);
#endif

#if PX_UART_CFG_UART5_EN
    uart_irq_handler(&px_uart_per_5);
#endif
}
#endif

static inline void px_uart_start_tx(USART_TypeDef * usart_base_adr)
{
    // Sanity check
    PX_DBG_ASSERT(usart_base_adr != NULL);

    // Enable Transmit data register empty interrupt
    LL_USART_EnableIT_TXE(usart_base_adr);
}

static bool px_uart_init_peripheral(USART_TypeDef *     usart_base_adr,
                                    px_uart_nr_t        uart_nr,
                                    uint32_t            baud,
                                    px_uart_data_bits_t data_bits,
                                    px_uart_parity_t    parity,
                                    px_uart_stop_bits_t stop_bits)
{
    // Sanity check
    PX_DBG_ASSERT(usart_base_adr != NULL);

    // USART_CR1 register calculated value
    uint32_t usart_cr1_val;

    // Enable peripheral clock
    switch(uart_nr)
    {
#if PX_UART_CFG_UART1_EN
    case PX_UART_NR_1:
        LL_APB2_GRP1_EnableClock(LL_APB2_GRP1_PERIPH_USART1);
        break;
#endif
#if PX_UART_CFG_UART2_EN
    case PX_UART_NR_2:
        LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);
        break;
#endif
#if PX_UART_CFG_UART3_EN
    case PX_UART_NR_3:
        LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART3);
        break;
#endif
#if PX_UART_CFG_UART4_EN
    case PX_UART_NR_4:
        LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART4);
        break;
#endif
#if PX_UART_CFG_UART5_EN
    case PX_UART_NR_5:
        LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART5);
        break;
#endif
    default:
        PX_DBG_ERR("Invalid peripheral");
        return false;
    }

    // Set baud rate
    LL_USART_SetBaudRate(usart_base_adr,
                         PX_BOARD_PER_CLK_HZ,
#if STM32G0
                         LL_USART_PRESCALER_DIV1,
#endif
                         LL_USART_OVERSAMPLING_16,
                         baud);

    // Set transmitter and receiver and receive interrupt
#if STM32G0
    usart_cr1_val = USART_CR1_RXNEIE_RXFNEIE | USART_CR1_TE | USART_CR1_RE;
#else
    usart_cr1_val = USART_CR1_RXNEIE | USART_CR1_TE | USART_CR1_RE;
#endif
    // Parity specified?
    if(parity != PX_UART_PARITY_NONE)
    {
        // Increment data bits to include parity bit
        if(data_bits == PX_UART_DATA_BITS_7)
        {
            data_bits = PX_UART_DATA_BITS_8;
        }
        else if(data_bits == PX_UART_DATA_BITS_8)
        {
            data_bits = PX_UART_DATA_BITS_9;
        }
        else
        {
            PX_DBG_ERR("Parity option invalid");
        }
    }
#ifdef STM32L1
    // Set number of data bits
    switch(data_bits)
    {
    case PX_UART_DATA_BITS_8:
        break;
    case PX_UART_DATA_BITS_9:
        usart_cr1_val |= USART_CR1_M;
        break;
    default:
        PX_DBG_ERR("Invalid number of data bits");
        return false;
    }
#else
    // Set number of data bits
    switch(data_bits)
    {
    case PX_UART_DATA_BITS_7:
        usart_cr1_val |= USART_CR1_M1;
        break;
    case PX_UART_DATA_BITS_8:
        break;
    case PX_UART_DATA_BITS_9:
        usart_cr1_val |= USART_CR1_M0;
        break;
    default:
        PX_DBG_ERR("Invalid number of data bits");
        return false;
    }
#endif
    // Set parity
    switch(parity)
    {
    case PX_UART_PARITY_NONE:
        break;
    case PX_UART_PARITY_ODD:
        usart_cr1_val |= USART_CR1_PCE | USART_CR1_PS;
        break;
    case PX_UART_PARITY_EVEN:
        usart_cr1_val |= USART_CR1_PCE;
        break;
    default:
        PX_DBG_ERR("Invalid parity specified");
        return false;
    }
    // Set stop bits
    switch(stop_bits)
    {
    case PX_UART_STOP_BITS_1:
        LL_USART_SetStopBitsLength(usart_base_adr, LL_USART_STOPBITS_1);
        break;
    case PX_UART_STOP_BITS_2:
        LL_USART_SetStopBitsLength(usart_base_adr, LL_USART_STOPBITS_2);
        break;
    default:
        PX_DBG_ERR("Invalid number of stop bits specified");
        return false;
    }
    // Set Control register 1
    usart_base_adr->CR1 = usart_cr1_val;
    // Enable UART
    LL_USART_Enable(usart_base_adr);
    // Enable interrupt handler
    switch(uart_nr)
    {
#if PX_UART_CFG_UART1_EN
    case PX_UART_NR_1:
        NVIC_EnableIRQ(USART1_IRQn);
        break;
#endif
#if PX_UART_CFG_UART2_EN
    case PX_UART_NR_2:
        NVIC_EnableIRQ(USART2_IRQn);
        break;
#endif
#if PX_UART_CFG_UART3_EN
    case PX_UART_NR_3:
        NVIC_EnableIRQ(USART3_IRQn);
        break;
#endif
#if PX_UART_CFG_UART4_EN
    case PX_UART_NR_4:
        NVIC_EnableIRQ(USART4_5_IRQn);
        break;
#endif
#if PX_UART_CFG_UART5_EN
    case PX_UART_NR_5:
        NVIC_EnableIRQ(USART4_5_IRQn);
        break;
#endif
    default:
        break;
    }
    // Success
    return true;
}

static void px_uart_init_peripheral_data(px_uart_nr_t    uart_nr,
                                         px_uart_per_t * uart_per)
{
    // Set peripheral
    uart_per->uart_nr = uart_nr;

    // Set peripheral base address and intialise circular buffers
    switch(uart_nr)
    {
#if PX_UART_CFG_UART1_EN
    case PX_UART_NR_1:
        uart_per->usart_base_adr = USART1;
        px_ring_buf_init(&uart_per->tx_circ_buf,
                         px_uart1_tx_circ_buf_data,
                         PX_UART_CFG_UART1_TX_BUF_SIZE);
        px_ring_buf_init(&uart_per->rx_circ_buf,
                         px_uart1_rx_circ_buf_data,
                         PX_UART_CFG_UART1_RX_BUF_SIZE);
        break;
#endif
#if PX_UART_CFG_UART2_EN
    case PX_UART_NR_2:
        uart_per->usart_base_adr = USART2;
        px_ring_buf_init(&uart_per->tx_circ_buf,
                         px_uart2_tx_circ_buf_data,
                         PX_UART_CFG_UART2_TX_BUF_SIZE);
        px_ring_buf_init(&uart_per->rx_circ_buf,
                         px_uart2_rx_circ_buf_data,
                         PX_UART_CFG_UART2_RX_BUF_SIZE);
        break;
#endif
#if PX_UART_CFG_UART3_EN
    case PX_UART_NR_3:
        uart_per->usart_base_adr = USART3;
        px_ring_buf_init(&uart_per->tx_circ_buf,
                         px_uart3_tx_circ_buf_data,
                         PX_UART_CFG_UART3_TX_BUF_SIZE);
        px_ring_buf_init(&uart_per->rx_circ_buf,
                         px_uart3_rx_circ_buf_data,
                         PX_UART_CFG_UART3_RX_BUF_SIZE);
        break;
#endif
#if PX_UART_CFG_UART4_EN
    case PX_UART_NR_4:
        uart_per->usart_base_adr = USART4;
        px_ring_buf_init(&uart_per->tx_circ_buf,
                         px_uart4_tx_circ_buf_data,
                         PX_UART_CFG_UART4_TX_BUF_SIZE);
        px_ring_buf_init(&uart_per->rx_circ_buf,
                         px_uart4_rx_circ_buf_data,
                         PX_UART_CFG_UART4_RX_BUF_SIZE);
        break;
#endif
#if PX_UART_CFG_UART5_EN
    case PX_UART_NR_5:
        uart_per->usart_base_adr = USART5;
        px_ring_buf_init(&uart_per->tx_circ_buf,
                         px_uart5_tx_circ_buf_data,
                         PX_UART_CFG_UART5_TX_BUF_SIZE);
        px_ring_buf_init(&uart_per->rx_circ_buf,
                         px_uart5_rx_circ_buf_data,
                         PX_UART_CFG_UART5_RX_BUF_SIZE);
        break;
#endif
    default:
        break;
    }
    // Clear open counter
    uart_per->open_counter = 0;
    // Set transmit done flag
    uart_per->tx_done = true;
}

/* _____GLOBAL FUNCTIONS_____________________________________________________ */
void px_uart_init(void)
{
    // Initialize peripheral data for each enabled peripheral
#if PX_UART_CFG_UART1_EN
    px_uart_init_peripheral_data(PX_UART_NR_1, &px_uart_per_1);
#endif
#if PX_UART_CFG_UART2_EN
    px_uart_init_peripheral_data(PX_UART_NR_2, &px_uart_per_2);
#endif
#if PX_UART_CFG_UART3_EN
    px_uart_init_peripheral_data(PX_UART_NR_3, &px_uart_per_3);
#endif
#if PX_UART_CFG_UART4_EN
    px_uart_init_peripheral_data(PX_UART_NR_4, &px_uart_per_4);
#endif
#if PX_UART_CFG_UART5_EN
    px_uart_init_peripheral_data(PX_UART_NR_5, &px_uart_per_5);
#endif
}

bool px_uart_open(px_uart_handle_t * handle,
                  px_uart_nr_t       uart_nr)
{
    return px_uart_open2(handle,
                         uart_nr,
                         PX_UART_CFG_DEFAULT_BAUD,
                         PX_UART_DATA_BITS_8,
                         PX_UART_PARITY_NONE,
                         PX_UART_STOP_BITS_1);
}

bool px_uart_open2(px_uart_handle_t *  handle,
                   px_uart_nr_t        uart_nr,
                   uint32_t            baud,
                   px_uart_data_bits_t data_bits,
                   px_uart_parity_t    parity,
                   px_uart_stop_bits_t stop_bits)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Handle not initialised
    handle->uart_per = NULL;
    // Set pointer to peripheral data
    switch(uart_nr)
    {
#if PX_UART_CFG_UART1_EN
    case PX_UART_NR_1:
        uart_per = &px_uart_per_1;
        break;
#endif
#if PX_UART_CFG_UART2_EN
    case PX_UART_NR_2:
        uart_per = &px_uart_per_2;
        break;
#endif
#if PX_UART_CFG_UART3_EN
    case PX_UART_NR_3:
        uart_per = &px_uart_per_3;
        break;
#endif
#if PX_UART_CFG_UART4_EN
    case PX_UART_NR_4:
        uart_per = &px_uart_per_4;
        break;
#endif
#if PX_UART_CFG_UART5_EN
    case PX_UART_NR_5:
        uart_per = &px_uart_per_5;
        break;
#endif
    default:
        PX_DBG_ERR("Invalid peripheral specified");
        return false;
    }
    // Already open?
    if(uart_per->open_counter != 0)
    {
        PX_DBG_ERR("Only one handle per UART peripheral can be opened");
        return false;
    }
    // Set transmit done flag
    uart_per->tx_done = true;
    // Initialise peripheral
    if(!px_uart_init_peripheral(uart_per->usart_base_adr,
                                uart_nr,
                                baud,
                                data_bits,
                                parity,
                                stop_bits))
    {
        // Invalid parameter specified
        return false;
    }
    // Point handle to peripheral
    handle->uart_per = uart_per;
    // Success
    uart_per->open_counter = 1;
    return true;
}

bool px_uart_close(px_uart_handle_t * handle)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);

    // Already closed?
    if(uart_per->open_counter == 0)
    {
        PX_DBG_ERR("Peripheral already closed");
        return false;
    }

    // Disable interrupt handler
    switch(uart_per->uart_nr)
    {
#if PX_UART_CFG_UART1_EN
    case PX_UART_NR_1:
        NVIC_DisableIRQ(USART1_IRQn);
        break;
#endif

#if PX_UART_CFG_UART2_EN
    case PX_UART_NR_2:
        NVIC_DisableIRQ(USART2_IRQn);
        break;
#endif

#if PX_UART_CFG_UART3_EN
    case PX_UART_NR_3:
        NVIC_DisableIRQ(USART3_IRQn);
        break;
#endif

#if PX_UART_CFG_UART4_EN
    case PX_UART_NR_4:
#if PX_UART_CFG_UART5_EN
        if(px_uart_per_5.open_counter == 0)
#endif
        {
            NVIC_DisableIRQ(USART4_5_IRQn);
        }
        break;
#endif

#if PX_UART_CFG_UART5_EN
    case PX_UART_NR_5:
#if PX_UART_CFG_UART4_EN
        if(px_uart_per_4.open_counter == 0)
#endif
        {
            NVIC_DisableIRQ(USART4_5_IRQn);
        }
        break;
#endif

    default:
        return false;
    }

    // Disable peripheral
    uart_per->usart_base_adr->CR1 = 0;

    // Disable peripheral clock
    switch(uart_per->uart_nr)
    {
#if PX_UART_CFG_UART1_EN
    case PX_UART_NR_1:
        LL_APB2_GRP1_DisableClock(LL_APB2_GRP1_PERIPH_USART1);
        break;
#endif
#if PX_UART_CFG_UART2_EN
    case PX_UART_NR_2:
        LL_APB1_GRP1_DisableClock(LL_APB1_GRP1_PERIPH_USART2);
        break;
#endif
#if PX_UART_CFG_UART3_EN
    case PX_UART_NR_3:
        LL_APB1_GRP1_DisableClock(LL_APB1_GRP1_PERIPH_USART2);
        break;
#endif
#if PX_UART_CFG_UART4_EN
    case PX_UART_NR_4:
        LL_APB1_GRP1_DisableClock(LL_APB1_GRP1_PERIPH_USART4);
        break;
#endif
#if PX_UART_CFG_UART5_EN
    case PX_UART_NR_5:
        LL_APB1_GRP1_DisableClock(LL_APB1_GRP1_PERIPH_USART5);
        break;
#endif
    default:
        PX_DBG_ERR("Invalid peripheral");
        return false;
    }

    // Close handle
    handle->uart_per = NULL;

    // Success
    uart_per->open_counter = 0;
    return true;
}

void px_uart_put_char(px_uart_handle_t * handle, char data)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Wait until transmit buffer has space for one byte and add it
    while(!px_ring_buf_wr_u8(&uart_per->tx_circ_buf, (uint8_t)data))
    {
        ;
    }

    // Make sure transmit process is started by enabling interrupt
    px_uart_start_tx(uart_per->usart_base_adr);
}

bool px_uart_wr_u8(px_uart_handle_t * handle, uint8_t data)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Add byte to transmit buffer
    if(!px_ring_buf_wr_u8(&uart_per->tx_circ_buf, (uint8_t)data))
    {
        // Could not buffer byte for transmission
        return false;
    }

    // Make sure transmit process is started by enabling interrupt
    px_uart_start_tx(uart_per->usart_base_adr);

    // Success
    return true;
}

size_t px_uart_wr(px_uart_handle_t * handle,
                  const void *       data,
                  size_t             nr_of_bytes)
{
    px_uart_per_t * uart_per;
    uint8_t  *      data_u8 = (uint8_t *)data;
    size_t          bytes_buffered = 0;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Add bytes to transmit buffer
    bytes_buffered = px_ring_buf_wr(&uart_per->tx_circ_buf,
                                    data_u8,
                                    nr_of_bytes);

    if(bytes_buffered > 0)
    {
        // Make sure transmit process is started by enabling interrupt
        px_uart_start_tx(uart_per->usart_base_adr);
    }

    // Indicate number of bytes buffered
    return bytes_buffered;
}

char px_uart_get_char(px_uart_handle_t * handle)
{
    px_uart_per_t * uart_per;
    uint8_t         data;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Wait until a byte is in receive buffer and fetch it
    while(!px_ring_buf_rd_u8(&uart_per->rx_circ_buf, &data))
    {
        ;
    }

    return (char)data;
}

bool px_uart_rd_u8(px_uart_handle_t * handle, uint8_t * data)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Return byte from receive buffer (if it is available)
    return px_ring_buf_rd_u8(&uart_per->rx_circ_buf, data);
}

size_t px_uart_rd(px_uart_handle_t * handle,
                  void *             buffer,
                  size_t             nr_of_bytes)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Fetch data from receive buffer (up to the specified number of bytes)
    return px_ring_buf_rd(&uart_per->rx_circ_buf,
                               buffer,
                               nr_of_bytes);
}

bool px_uart_wr_buf_is_full(px_uart_handle_t * handle)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    return px_ring_buf_is_full(&uart_per->tx_circ_buf);
}

bool px_uart_wr_buf_is_empty(px_uart_handle_t * handle)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    return px_ring_buf_is_empty(&uart_per->tx_circ_buf);
}

bool px_uart_wr_is_done(px_uart_handle_t * handle)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Any data to be transmitted in buffer?
    if(!px_ring_buf_is_empty(&uart_per->tx_circ_buf))
    {
        return false;
    }
    // Return transmission done flag
    return uart_per->tx_done;
}

bool px_uart_rd_buf_is_empty(px_uart_handle_t * handle)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    return px_ring_buf_is_empty(&uart_per->rx_circ_buf);
}

void px_uart_ioctl_change_baud(px_uart_handle_t * handle, uint32_t baud)
{
    px_uart_per_t * uart_per;

    // Verify that pointer to handle is not NULL
    PX_DBG_ASSERT(handle != NULL);
    // Set pointer to peripheral
    uart_per = handle->uart_per;
    // Check that handle is open
    PX_DBG_ASSERT(uart_per != NULL);
    PX_DBG_ASSERT(uart_per->open_counter != 0);
    PX_DBG_ASSERT(uart_per->usart_base_adr != NULL);

    // Set baud rate
    LL_USART_SetBaudRate(uart_per->usart_base_adr,
                         PX_BOARD_PER_CLK_HZ,
#if STM32G0
                         LL_USART_PRESCALER_DIV1,
#endif
                         LL_USART_OVERSAMPLING_16,
                         baud);
}
