/*
 * Copyright (c) 2017, NXP
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <device.h>
#include <uart.h>
#include <fsl_uart.h>
#include <fsl_clock.h>
#include <soc.h>

struct uart_mcux_config {
	UART_Type *base;
	clock_name_t clock_source;
	u32_t baud_rate;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	void (*irq_config_func)(struct device *dev);
#endif
};

struct uart_mcux_data {
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_t callback;
#endif
};

static int uart_mcux_poll_in(struct device *dev, unsigned char *c)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t flags = UART_GetStatusFlags(config->base);
	int ret = -1;

	if (flags & kUART_RxDataRegFullFlag) {
		*c = UART_ReadByte(config->base);
		ret = 0;
	}

	return ret;
}

static unsigned char uart_mcux_poll_out(struct device *dev, unsigned char c)
{
	const struct uart_mcux_config *config = dev->config->config_info;

	while (!(UART_GetStatusFlags(config->base) & kUART_TxDataRegEmptyFlag))
		;

	UART_WriteByte(config->base, c);

	return c;
}

static int uart_mcux_err_check(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t flags = UART_GetStatusFlags(config->base);
	int err = 0;

	if (flags & kUART_RxOverrunFlag) {
		err |= UART_ERROR_OVERRUN;
	}

	if (flags & kUART_ParityErrorFlag) {
		err |= UART_ERROR_PARITY;
	}

	if (flags & kUART_FramingErrorFlag) {
		err |= UART_ERROR_FRAMING;
	}

	UART_ClearStatusFlags(config->base, kUART_RxOverrunFlag |
					    kUART_ParityErrorFlag |
					    kUART_FramingErrorFlag);

	return err;
}

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static int uart_mcux_fifo_fill(struct device *dev, const u8_t *tx_data,
			       int len)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u8_t num_tx = 0;

	while ((len - num_tx > 0) &&
	       (UART_GetStatusFlags(config->base) & kUART_TxDataRegEmptyFlag)) {

		UART_WriteByte(config->base, tx_data[num_tx++]);
	}

	return num_tx;
}

static int uart_mcux_fifo_read(struct device *dev, u8_t *rx_data,
			       const int len)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u8_t num_rx = 0;

	while ((len - num_rx > 0) &&
	       (UART_GetStatusFlags(config->base) & kUART_RxDataRegFullFlag)) {

		rx_data[num_rx++] = UART_ReadByte(config->base);
	}

	return num_rx;
}

static void uart_mcux_irq_tx_enable(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_TxDataRegEmptyInterruptEnable;

	UART_EnableInterrupts(config->base, mask);
}

static void uart_mcux_irq_tx_disable(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_TxDataRegEmptyInterruptEnable;

	UART_DisableInterrupts(config->base, mask);
}

static int uart_mcux_irq_tx_empty(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t flags = UART_GetStatusFlags(config->base);

	return (flags & kUART_TxDataRegEmptyFlag) != 0;
}

static int uart_mcux_irq_tx_ready(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_TxDataRegEmptyInterruptEnable;

	return (UART_GetEnabledInterrupts(config->base) & mask)
		&& uart_mcux_irq_tx_empty(dev);
}

static void uart_mcux_irq_rx_enable(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_RxDataRegFullInterruptEnable;

	UART_EnableInterrupts(config->base, mask);
}

static void uart_mcux_irq_rx_disable(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_RxDataRegFullInterruptEnable;

	UART_DisableInterrupts(config->base, mask);
}

static int uart_mcux_irq_rx_full(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t flags = UART_GetStatusFlags(config->base);

	return (flags & kUART_RxDataRegFullFlag) != 0;
}

static int uart_mcux_irq_rx_ready(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_RxDataRegFullInterruptEnable;

	return (UART_GetEnabledInterrupts(config->base) & mask)
		&& uart_mcux_irq_rx_full(dev);
}

static void uart_mcux_irq_err_enable(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_NoiseErrorInterruptEnable |
			kUART_FramingErrorInterruptEnable |
			kUART_ParityErrorInterruptEnable;

	UART_EnableInterrupts(config->base, mask);
}

static void uart_mcux_irq_err_disable(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	u32_t mask = kUART_NoiseErrorInterruptEnable |
			kUART_FramingErrorInterruptEnable |
			kUART_ParityErrorInterruptEnable;

	UART_DisableInterrupts(config->base, mask);
}

static int uart_mcux_irq_is_pending(struct device *dev)
{
	return uart_mcux_irq_tx_ready(dev) || uart_mcux_irq_rx_ready(dev);
}

static int uart_mcux_irq_update(struct device *dev)
{
	return 1;
}

static void uart_mcux_irq_callback_set(struct device *dev,
				       uart_irq_callback_t cb)
{
	struct uart_mcux_data *data = dev->driver_data;

	data->callback = cb;
}

static void uart_mcux_isr(void *arg)
{
	struct device *dev = arg;
	struct uart_mcux_data *data = dev->driver_data;

	if (data->callback) {
		data->callback(dev);
	}
}
#endif /* CONFIG_UART_INTERRUPT_DRIVEN */

static int uart_mcux_init(struct device *dev)
{
	const struct uart_mcux_config *config = dev->config->config_info;
	uart_config_t uart_config;
	u32_t clock_freq;

	clock_freq = CLOCK_GetFreq(config->clock_source);

	UART_GetDefaultConfig(&uart_config);
	uart_config.enableTx = true;
	uart_config.enableRx = true;
	uart_config.baudRate_Bps = config->baud_rate;

	UART_Init(config->base, &uart_config, clock_freq);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	config->irq_config_func(dev);
#endif

	return 0;
}

static const struct uart_driver_api uart_mcux_driver_api = {
	.poll_in = uart_mcux_poll_in,
	.poll_out = uart_mcux_poll_out,
	.err_check = uart_mcux_err_check,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.fifo_fill = uart_mcux_fifo_fill,
	.fifo_read = uart_mcux_fifo_read,
	.irq_tx_enable = uart_mcux_irq_tx_enable,
	.irq_tx_disable = uart_mcux_irq_tx_disable,
	.irq_tx_empty = uart_mcux_irq_tx_empty,
	.irq_tx_ready = uart_mcux_irq_tx_ready,
	.irq_rx_enable = uart_mcux_irq_rx_enable,
	.irq_rx_disable = uart_mcux_irq_rx_disable,
	.irq_rx_ready = uart_mcux_irq_rx_ready,
	.irq_err_enable = uart_mcux_irq_err_enable,
	.irq_err_disable = uart_mcux_irq_err_disable,
	.irq_is_pending = uart_mcux_irq_is_pending,
	.irq_update = uart_mcux_irq_update,
	.irq_callback_set = uart_mcux_irq_callback_set,
#endif
};

#ifdef CONFIG_UART_MCUX_0

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_0(struct device *dev);
#endif

static const struct uart_mcux_config uart_mcux_0_config = {
	.base = UART0,
	.clock_source = UART0_CLK_SRC,
	.baud_rate = CONFIG_UART_MCUX_0_BAUD_RATE,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.irq_config_func = uart_mcux_config_func_0,
#endif
};

static struct uart_mcux_data uart_mcux_0_data;

DEVICE_AND_API_INIT(uart_0, CONFIG_UART_MCUX_0_NAME,
		    &uart_mcux_init,
		    &uart_mcux_0_data, &uart_mcux_0_config,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_mcux_driver_api);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_0(struct device *dev)
{
	IRQ_CONNECT(IRQ_UART0_STATUS, CONFIG_UART_MCUX_0_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_0), 0);

	irq_enable(IRQ_UART0_STATUS);

#ifdef IRQ_UART0_ERROR
	IRQ_CONNECT(IRQ_UART0_ERROR, CONFIG_UART_MCUX_0_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_0), 0);

	irq_enable(IRQ_UART0_ERROR);
#endif
}
#endif

#endif /* CONFIG_UART_MCUX_0 */

#ifdef CONFIG_UART_MCUX_1

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_1(struct device *dev);
#endif

static const struct uart_mcux_config uart_mcux_1_config = {
	.base = UART1,
	.clock_source = UART1_CLK_SRC,
	.baud_rate = CONFIG_UART_MCUX_1_BAUD_RATE,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.irq_config_func = uart_mcux_config_func_1,
#endif
};

static struct uart_mcux_data uart_mcux_1_data;

DEVICE_AND_API_INIT(uart_1, CONFIG_UART_MCUX_1_NAME,
		    &uart_mcux_init,
		    &uart_mcux_1_data, &uart_mcux_1_config,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_mcux_driver_api);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_1(struct device *dev)
{
	IRQ_CONNECT(IRQ_UART1_STATUS, CONFIG_UART_MCUX_1_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_1), 0);

	irq_enable(IRQ_UART1_STATUS);

#ifdef IRQ_UART1_ERROR
	IRQ_CONNECT(IRQ_UART1_ERROR, CONFIG_UART_MCUX_1_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_1), 0);

	irq_enable(IRQ_UART1_ERROR);
#endif
}
#endif

#endif /* CONFIG_UART_MCUX_1 */

#ifdef CONFIG_UART_MCUX_2

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_2(struct device *dev);
#endif

static const struct uart_mcux_config uart_mcux_2_config = {
	.base = UART2,
	.clock_source = UART2_CLK_SRC,
	.baud_rate = CONFIG_UART_MCUX_2_BAUD_RATE,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.irq_config_func = uart_mcux_config_func_2,
#endif
};

static struct uart_mcux_data uart_mcux_2_data;

DEVICE_AND_API_INIT(uart_2, CONFIG_UART_MCUX_2_NAME,
		    &uart_mcux_init,
		    &uart_mcux_2_data, &uart_mcux_2_config,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_mcux_driver_api);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_2(struct device *dev)
{
	IRQ_CONNECT(IRQ_UART2_STATUS, CONFIG_UART_MCUX_2_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_2), 0);

	irq_enable(IRQ_UART2_STATUS);

#ifdef IRQ_UART2_ERROR
	IRQ_CONNECT(IRQ_UART2_ERROR, CONFIG_UART_MCUX_2_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_2), 0);

	irq_enable(IRQ_UART2_ERROR);
#endif
}
#endif

#endif /* CONFIG_UART_MCUX_2 */

#ifdef CONFIG_UART_MCUX_3

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_3(struct device *dev);
#endif

static const struct uart_mcux_config uart_mcux_3_config = {
	.base = UART3,
	.clock_source = UART3_CLK_SRC,
	.baud_rate = CONFIG_UART_MCUX_3_BAUD_RATE,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.irq_config_func = uart_mcux_config_func_3,
#endif
};

static struct uart_mcux_data uart_mcux_3_data;

DEVICE_AND_API_INIT(uart_3, CONFIG_UART_MCUX_3_NAME,
		    &uart_mcux_init,
		    &uart_mcux_3_data, &uart_mcux_3_config,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_mcux_driver_api);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_3(struct device *dev)
{
	IRQ_CONNECT(IRQ_UART3_STATUS, CONFIG_UART_MCUX_3_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_3), 0);

	irq_enable(IRQ_UART3_STATUS);

#ifdef IRQ_UART3_ERROR
	IRQ_CONNECT(IRQ_UART3_ERROR, CONFIG_UART_MCUX_3_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_3), 0);

	irq_enable(IRQ_UART3_ERROR);
#endif
}
#endif

#endif /* CONFIG_UART_MCUX_3 */

#ifdef CONFIG_UART_MCUX_4

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_4(struct device *dev);
#endif

static const struct uart_mcux_config uart_mcux_4_config = {
	.base = UART4,
	.clock_source = UART4_CLK_SRC,
	.baud_rate = CONFIG_UART_MCUX_4_BAUD_RATE,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.irq_config_func = uart_mcux_config_func_4,
#endif
};

static struct uart_mcux_data uart_mcux_4_data;

DEVICE_AND_API_INIT(uart_4, CONFIG_UART_MCUX_4_NAME,
		    &uart_mcux_init,
		    &uart_mcux_4_data, &uart_mcux_4_config,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_mcux_driver_api);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_4(struct device *dev)
{
	IRQ_CONNECT(IRQ_UART4_STATUS, CONFIG_UART_MCUX_4_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_4), 0);

	irq_enable(IRQ_UART4_STATUS);

#ifdef IRQ_UART4_ERROR
	IRQ_CONNECT(IRQ_UART4_ERROR, CONFIG_UART_MCUX_4_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_4), 0);

	irq_enable(IRQ_UART4_ERROR);
#endif
}
#endif

#endif /* CONFIG_UART_MCUX_4 */

#ifdef CONFIG_UART_MCUX_5

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_5(struct device *dev);
#endif

static const struct uart_mcux_config uart_mcux_5_config = {
	.base = UART5,
	.clock_source = UART5_CLK_SRC,
	.baud_rate = CONFIG_UART_MCUX_5_BAUD_RATE,
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	.irq_config_func = uart_mcux_config_func_5,
#endif
};

static struct uart_mcux_data uart_mcux_5_data;

DEVICE_AND_API_INIT(uart_5, CONFIG_UART_MCUX_5_NAME,
		    &uart_mcux_init,
		    &uart_mcux_5_data, &uart_mcux_5_config,
		    PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &uart_mcux_driver_api);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_mcux_config_func_5(struct device *dev)
{
	IRQ_CONNECT(IRQ_UART5_STATUS, CONFIG_UART_MCUX_5_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_5), 0);

	irq_enable(IRQ_UART5_STATUS);

#ifdef IRQ_UART5_ERROR
	IRQ_CONNECT(IRQ_UART5_ERROR, CONFIG_UART_MCUX_5_IRQ_PRI,
		    uart_mcux_isr, DEVICE_GET(uart_5), 0);

	irq_enable(IRQ_UART5_ERROR);
#endif
}
#endif

#endif /* CONFIG_UART_MCUX_5 */
