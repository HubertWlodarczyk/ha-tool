/*
 * Copyright (c) 2026 Hubert Włodarczyk
 *
 * SPDX-License-Identifier: MPL-2.0
 */

/*
 * nRF5340 DK only: the application core starts at 64 MHz (MDK: HFCLKCTRL = Div2) and upstream
 * Zephyr leaves it there. 128 MHz halves the time the host spends per ISO SDU, HCI packet and
 * CLI line, which matters with many BISes. Timers run from the RTC, unaffected.
 */

#include <zephyr/init.h>

#include <hal/nrf_clock.h>

static int cpu_128mhz(void)
{
	nrf_clock_hfclk_div_set(NRF_CLOCK, NRF_CLOCK_HFCLK_DIV_1);
	SystemCoreClockUpdate();

	return 0;
}

SYS_INIT(cpu_128mhz, PRE_KERNEL_1, 0);
