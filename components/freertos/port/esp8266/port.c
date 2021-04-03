/*
 * FreeRTOS Kernel V10.0.1
 * Copyright (C) 2017 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 * Additions Copyright 2018 Espressif Systems (Shanghai) PTE LTD
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * http://www.FreeRTOS.org
 * http://aws.amazon.com/freertos
 *
 * 1 tab == 4 spaces!
 */

/* Scheduler includes. */

#include <stdint.h>
#include <string.h>

#include <xtensa/config/core.h>
#include <xtensa/tie/xt_interrupt.h>
#include <xtensa/tie/xt_timer.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/xtensa_rtos.h"

#include "esp_attr.h"
#include "esp_libc.h"
#include "esp_task_wdt.h"
#include "esp_sleep.h"

#include "esp8266/eagle_soc.h"
#include "rom/ets_sys.h"
#include "esp8266/rom_functions.h"
#include "driver/soc.h"

#define SET_STKREG(r,v)     sp[(r) >> 2] = (uint32_t)(v)
#define PORT_ASSERT(x)      do { if (!(x)) {ets_printf("%s %u\n", "rtos_port", __LINE__); while(1){}; }} while (0)

extern uint8_t NMIIrqIsOn;

uint32_t cpu_sr;

uint32_t _xt_tick_divisor;

/* Each task maintains its own interrupt status in the critical nesting
variable. */
static uint32_t uxCriticalNesting = 0;

uint32_t g_esp_boot_ccount;
uint64_t g_esp_os_us;

#ifdef ESP_ENABLE_ESP_OS_TICKS
/* apparently unused for at least a year. remove to free resources and increase performance  */
uint64_t g_esp_os_ticks;
#endif

#ifndef CONFIG_FREERTOS_RUN_TIME_STATS_USING_CPU_CLK
uint64_t g_esp_os_cpu_clk;
#endif
static uint32_t s_switch_ctx_flag;

void vPortEnterCritical(void);
void vPortExitCritical(void);

void IRAM_ATTR portYIELD_FROM_ISR(void)
{
    s_switch_ctx_flag = 1;
}

uint8_t *__cpu_init_stk(uint8_t *stack_top, void (*_entry)(void *), void *param, void (*_exit)(void))
{

    uint32_t *sp, *tp, *stk = (uint32_t *)stack_top;

    /* Create interrupt stack frame aligned to 16 byte boundary */
    sp = (uint32_t *)(((uint32_t)(stk + 1) - XT_CP_SIZE - XT_STK_FRMSZ) & ~0xf);

    /* Clear the entire frame (do not use memset() because we don't depend on C library) */
    for (tp = sp; tp <= stk; ++tp) {
        *tp = 0;
    }

    /* Explicitly initialize certain saved registers */
    SET_STKREG(XT_STK_PC,   _entry);                        /* task entrypoint                  */
    SET_STKREG(XT_STK_A0,   _exit);                         /* to terminate GDB backtrace       */
    SET_STKREG(XT_STK_A1,   (uint32_t)sp + XT_STK_FRMSZ);   /* physical top of stack frame      */
    SET_STKREG(XT_STK_A2,   param);                         /* parameters      */
    SET_STKREG(XT_STK_EXIT, _xt_user_exit);                 /* user exception exit dispatcher   */

    /* Set initial PS to int level 0, EXCM disabled ('rfe' will enable), user mode. */
    SET_STKREG(XT_STK_PS,      PS_UM | PS_EXCM);

    return (uint8_t *)sp;
}

#ifndef DISABLE_FREERTOS
/*
 * See header file for description.
 */
StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack, pdTASK_CODE pxCode, void *pvParameters)
{
    return (StackType_t *)__cpu_init_stk((uint8_t *)pxTopOfStack, pxCode, pvParameters, NULL);
}
#endif

void IRAM_ATTR PendSV(int req)
{
    if (req == 1) {
        vPortEnterCritical();
        s_switch_ctx_flag = 1;
        xthal_set_intset(1 << ETS_SOFT_INUM);
        vPortExitCritical();
    } else if (req == 2) {
        xthal_set_intset(1 << ETS_SOFT_INUM);
    }
}

void IRAM_ATTR SoftIsrHdl(void* arg)
{
    extern int MacIsrSigPostDefHdl(void);

    if (MacIsrSigPostDefHdl()) {
        portYIELD_FROM_ISR();
    }
}

#ifdef ESP_ENABLE_ESP_OS_TICKS
void IRAM_ATTR esp_increase_tick_cnt(const TickType_t ticks)
{
	g_esp_os_ticks += ticks;
}
#endif

void IRAM_ATTR xPortSysTickHandle(void *p)
{
	uint32_t us, ticks;
	uint32_t ccount,  new_ccompare;
	int32_t elapsed;
	/**
	 * System or application may close interrupt too long, such as the operation of read/write/erase flash.
	 * And then the "ccount" value may be overflow.     *
	 * .
	 * - Reworked to avoid an heavy  drift in ccount management, in g_esp_os_us (esp_timer_get_time) and and xTickCount (xTaskGetTickCount)).
	 * - A "control loop" has been added to check the evaluated latency and protect against further latencies due to nmi (pwm driver)
	 * - The handler performances are increased by avoiding the math division at runtime in the main flow (about 150 fewer cpu ticks).
	 * - Others section are removed if not required by the current compile options.
	 * Finally, the maximum recoverable interrupt latency is close to: INT32_MAX ticks.  (>13 secs @160Mhz.) and ccount is
	 * no longer set to 0 at each handler call. This also restore the old feature that allow the sharing of ccount (read only) with other applications
	 */
	new_ccompare = soc_get_ccompare() + _xt_tick_divisor;
	ticks = 1;
	for (;;) {
		soc_set_ccompare(new_ccompare);
		ccount = soc_get_ccount();
		elapsed = (int32_t)(ccount - new_ccompare);
		/* check for the expected negative value or for a very unlikely new shot */
		if ((elapsed < 0) || (soc_get_int_mask() & (1ul << ETS_MAX_INUM))) {
			break;
		} else {
			/* too late. ccount will never fire again. need recover */
			uint32_t missedTicks = elapsed / _xt_tick_divisor +1;
			ticks += missedTicks;
			new_ccompare += missedTicks * _xt_tick_divisor;
		}
	}

	us = ticks * (1000000 / configTICK_RATE_HZ);
	g_esp_os_us += us;

#ifdef CONFIG_FREERTOS_RUN_TIME_STATS_USING_CPU_CLK
	if ((uint32_t) g_esp_os_cpu_clk > ccount) {
		/* ccount overflow */
		g_esp_os_cpu_clk += 0x100000000; //inc high part
	}
	g_esp_os_cpu_clk = (g_esp_os_cpu_clk & 0xFFFFFFFF00000000) | ccount; //low part overwrite
#endif

	if (ticks > 1) {
		vTaskStepTick(ticks - 1);
	}

#ifdef ESP_ENABLE_ESP_OS_TICKS
	g_esp_os_ticks++;
#endif

	if (xTaskIncrementTick() != pdFALSE) {
		portYIELD_FROM_ISR();
	}

}

/**
 * @brief Return current CPU clock frequency
 */
int esp_clk_cpu_freq(void)
{
    return _xt_tick_divisor * XT_TICK_PER_SEC;
}

/*
 * See header file for description.
 */
portBASE_TYPE xPortStartScheduler(void)
{
    /*
     * TAG 1.2.3 FreeRTOS call "portDISABLE_INTERRUPTS" at file tasks.c line 1973, this is not at old one.
     * This makes it to be a wrong value.
     * 
     * So we should initialize global value "cpu_sr" with a right value.
     * 
     * Todo: Remove this one when refactor startup function.
     */
    cpu_sr = 0x20;

    /*******software isr*********/
    _xt_isr_attach(ETS_SOFT_INUM, SoftIsrHdl, NULL);
    _xt_isr_unmask(1 << ETS_SOFT_INUM);

    _xt_isr_attach(ETS_MAX_INUM, xPortSysTickHandle, NULL);

    /* Initialize system tick timer interrupt and schedule the first tick. */
    _xt_tick_divisor = xtbsp_clock_freq_hz() / XT_TICK_PER_SEC;

    g_esp_boot_ccount = soc_get_ccount();
    soc_set_ccount(0);
    _xt_tick_timer_init();

    vTaskSwitchContext();

    /* Restore the context of the first task that is going to run. */
    _xt_enter_first_task();

    /* Should not get here as the tasks are now running! */
    return pdTRUE;
}

void vPortInitContextFromOldStack(StackType_t *newStackTop, StackType_t *oldStackTop, UBaseType_t stackSize)
{
    uintptr_t *sp;

    memcpy(newStackTop, oldStackTop, stackSize);
    sp = (uintptr_t *)newStackTop;
    sp[XT_STK_A1 / sizeof(uintptr_t)] = (uintptr_t)sp + XT_STK_FRMSZ;
}

void vPortEndScheduler(void)
{
    /* It is unlikely that the CM3 port will require this function as there
    is nothing to return to.  */
}
/*-----------------------------------------------------------*/

/*-----------------------------------------------------------*/

static char ClosedLv1Isr = 0;

void IRAM_ATTR vPortEnterCritical(void)
{
    if (NMIIrqIsOn == 0) {
        if (ClosedLv1Isr != 1) {
            portDISABLE_INTERRUPTS();
            ClosedLv1Isr = 1;
        }
        uxCriticalNesting++;
    }
}
/*-----------------------------------------------------------*/

void IRAM_ATTR vPortExitCritical(void)
{
    if (NMIIrqIsOn == 0) {
        if (uxCriticalNesting > 0) {
            uxCriticalNesting--;

            if (uxCriticalNesting == 0) {
                if (ClosedLv1Isr == 1) {
                    ClosedLv1Isr = 0;
                    portENABLE_INTERRUPTS();
                }
            }
        } else {
            ets_printf(DRAM_STR("E:C:%u\n"), uxCriticalNesting);
            PORT_ASSERT((uxCriticalNesting > 0));
        }
    }
}

void show_critical_info(void)
{
    ets_printf("ShowCritical:%u\n", uxCriticalNesting);
    ets_printf("s_switch_ctx_flag:%u\n", s_switch_ctx_flag);
}

#ifdef ESP_DPORT_CLOSE_NMI
static int s_nmi_is_closed;

void esp_dport_close_nmi(void)
{
    vPortEnterCritical();
    REG_WRITE(PERIPHS_DPORT_BASEADDR, REG_READ(PERIPHS_DPORT_BASEADDR) & ~0x1);
    s_nmi_is_closed = 1;
    vPortExitCritical();
}

#define ESP_NMI_IS_CLOSED()     s_nmi_is_closed
#else
#define ESP_NMI_IS_CLOSED()     0
#endif

void IRAM_ATTR vPortETSIntrLock(void)
{
    if (NMIIrqIsOn == 0) {
        uint32_t regval = REG_READ(NMI_INT_ENABLE_REG);

        vPortEnterCritical();

        REG_WRITE(NMI_INT_ENABLE_REG, 0);

        if (!ESP_NMI_IS_CLOSED()) {
            do {
                REG_WRITE(INT_ENA_WDEV, WDEV_TSF0_REACH_INT);
            } while(REG_READ(INT_ENA_WDEV) != WDEV_TSF0_REACH_INT);
        }

        REG_WRITE(NMI_INT_ENABLE_REG, regval);
    }
}

void IRAM_ATTR vPortETSIntrUnlock(void)
{
    if (NMIIrqIsOn == 0) {
        uint32_t regval = REG_READ(NMI_INT_ENABLE_REG);

        REG_WRITE(NMI_INT_ENABLE_REG, 0);

        if (!ESP_NMI_IS_CLOSED()) {
            extern uint32_t WDEV_INTEREST_EVENT;

            REG_WRITE(INT_ENA_WDEV, WDEV_INTEREST_EVENT);
        }

        REG_WRITE(NMI_INT_ENABLE_REG, regval);

        vPortExitCritical();
    }
}

/*
 * @brief check if CPU core interrupt is disable
 */
bool interrupt_is_disable(void)
{
    uint32_t tmp;

    __asm__ __volatile__ (
        "rsr %0, PS\n"
        : "=a"(tmp) : : "memory");

    return tmp & 0xFUL ? true : false;
}

static _xt_isr_entry s_isr[16];
static uint8_t s_xt_isr_status = 0;

void _xt_isr_attach(uint8_t i, _xt_isr func, void* arg)
{
    s_isr[i].handler = func;
    s_isr[i].arg = arg;
}

void IRAM_ATTR _xt_isr_handler(void)
{
    do {
        uint32_t mask = soc_get_int_mask();

        for (int i = 0; i < ETS_INT_MAX && mask; i++) {
            int bit = 1 << i;

            if (!(bit & mask) || !s_isr[i].handler)
                continue;

            soc_clear_int_mask(bit);

            s_xt_isr_status = 1;
            s_isr[i].handler(s_isr[i].arg);
            s_xt_isr_status = 0;

            mask &= ~bit;
        }
    } while (soc_get_int_mask());

    if (s_switch_ctx_flag) {
        vTaskSwitchContext();
        s_switch_ctx_flag = 0;
    }
}

int xPortInIsrContext(void)
{
    return s_xt_isr_status != 0;
}

void __attribute__((weak, noreturn)) vApplicationStackOverflowHook(xTaskHandle xTask, const char *pcTaskName)
{
    ets_printf("***ERROR*** A stack overflow in task %s has been detected.\r\n", pcTaskName);
    abort();
}

signed portBASE_TYPE xTaskGenericCreate(TaskFunction_t pxTaskCode,
                                        const signed char * const pcName,
                                        unsigned short usStackDepth,
                                        void *pvParameters,
                                        unsigned portBASE_TYPE uxPriority,
                                        TaskHandle_t *pxCreatedTask,
                                        StackType_t *puxStackBuffer,
                                        const MemoryRegion_t * const xRegions)
{
    (void)puxStackBuffer;
    (void)xRegions;
    return xTaskCreate(pxTaskCode, (const char * const)pcName, usStackDepth,
                       pvParameters, uxPriority, pxCreatedTask);
}

BaseType_t xQueueGenericReceive(QueueHandle_t xQueue, void * const pvBuffer,
                                TickType_t xTicksToWait, const BaseType_t xJustPeeking)
{
    configASSERT(xJustPeeking == 0);
    return xQueueReceive(xQueue, pvBuffer, xTicksToWait);
}

void esp_internal_idle_hook(void)
{
    esp_task_wdt_reset();

    esp_sleep_start();
}

#ifndef DISABLE_FREERTOS
#if configUSE_IDLE_HOOK == 1
void __attribute__((weak)) vApplicationIdleHook(void)
{

}
#endif

#if configUSE_TICK_HOOK == 1
void __attribute__((weak)) vApplicationTickHook(void)
{

}
#endif
#endif

uint32_t xPortGetTickRateHz(void)
{
    return (uint32_t)configTICK_RATE_HZ;
}
