#include <stdint.h>
#include "kernel.h"
#include "mutex.h"
#include "semaphore.h"
#include "app_config.h"


// ─── Base Addresses ───────────────────────────────────────────
#define RCC_BASE     0x40021000
#define FLASH_BASE   0x40022000
#define GPIOA_BASE   0x40010800
#define GPIOC_BASE   0x40011000
#define AFIO_BASE    0x40010000
#define EXTI_BASE    0x40010400
#define USART2_BASE  0x40004400
#define ADC1_BASE    0x40012400

#define RCC_CR       (*(volatile uint32_t *)(RCC_BASE + 0x00))
#define RCC_CFGR     (*(volatile uint32_t *)(RCC_BASE + 0x04))
#define RCC_APB2ENR  (*(volatile uint32_t *)(RCC_BASE + 0x18))
#define RCC_APB1ENR  (*(volatile uint32_t *)(RCC_BASE + 0x1C))
#define FLASH_ACR    (*(volatile uint32_t *)(FLASH_BASE + 0x00))
#define GPIOA_CRL    (*(volatile uint32_t *)(GPIOA_BASE + 0x00))
#define GPIOA_BSRR   (*(volatile uint32_t *)(GPIOA_BASE + 0x10))
#define GPIOC_CRH    (*(volatile uint32_t *)(GPIOC_BASE + 0x04))
#define GPIOC_ODR    (*(volatile uint32_t *)(GPIOC_BASE + 0x0C))
#define GPIOC_IDR    (*(volatile uint32_t *)(GPIOC_BASE + 0x08))
#define AFIO_EXTICR4 (*(volatile uint32_t *)(AFIO_BASE + 0x14))
#define EXTI_IMR     (*(volatile uint32_t *)(EXTI_BASE + 0x00))
#define EXTI_FTSR    (*(volatile uint32_t *)(EXTI_BASE + 0x0C))
#define EXTI_PR      (*(volatile uint32_t *)(EXTI_BASE + 0x14))
#define SYST_CSR     (*(volatile uint32_t *)0xE000E010)
#define SYST_RVR     (*(volatile uint32_t *)0xE000E014)
#define SYST_CVR     (*(volatile uint32_t *)0xE000E018)
#define NVIC_ISER1   (*(volatile uint32_t *)0xE000E104)
#define USART2_SR    (*(volatile uint32_t *)(USART2_BASE + 0x00))
#define USART2_DR    (*(volatile uint32_t *)(USART2_BASE + 0x04))
#define USART2_BRR   (*(volatile uint32_t *)(USART2_BASE + 0x08))
#define USART2_CR1   (*(volatile uint32_t *)(USART2_BASE + 0x0C))
#define USART2_CR2   (*(volatile uint32_t *)(USART2_BASE + 0x10))
#define ADC1_SR      (*(volatile uint32_t *)(ADC1_BASE + 0x00))
#define ADC1_CR1     (*(volatile uint32_t *)(ADC1_BASE + 0x04))
#define ADC1_CR2     (*(volatile uint32_t *)(ADC1_BASE + 0x08))
#define ADC1_SMPR1   (*(volatile uint32_t *)(ADC1_BASE + 0x0C))
#define ADC1_SMPR2   (*(volatile uint32_t *)(ADC1_BASE + 0x10))
#define ADC1_SQR1    (*(volatile uint32_t *)(ADC1_BASE + 0x2C))
#define ADC1_SQR3    (*(volatile uint32_t *)(ADC1_BASE + 0x34))
#define ADC1_DR      (*(volatile uint32_t *)(ADC1_BASE + 0x4C))

#define LED_PIN      5
#define BTN_PIN      13

typedef enum { STATE_LED_OFF = 1, STATE_LED_ON = 2, STATE_LED_BLINK = 3 } LedState;
typedef enum { WAIT_PRESS, DEBOUNCE, WAIT_RELEASE } DebounceState;

volatile uint32_t tick_count = 0;
volatile uint8_t  btn_event  = 0;
volatile uint8_t  rec_data   = 0;

/*
 * Chapter 28 demonstration mutex, and Chapter 29 demonstration
 * semaphore -- both are part of APP_MODE_APPLICATION only (see
 * app_config.h). The test-mode builds (APP_MODE_TEST_BASIC /
 * APP_MODE_TEST_PRIORITY) declare and use their own, separate
 * Semaphore_t instances inside semaphore_tests_basic.c /
 * semaphore_tests_priority.c instead of these.
 */
#if ACTIVE_APP_MODE == APP_MODE_APPLICATION

/*
 * USART2 is a single shared resource (one peripheral, one transmit
 * register) that ledTask, tempTask, and commsTask all write log
 * lines to. Without a mutex, a preemption in the middle of e.g.
 * usart2_send_string("Temp: ...") could let a higher-priority task's
 * own log line get spliced into the middle of it -- interleaved,
 * garbled output. This mutex makes "print one log line" atomic with
 * respect to the other tasks, exactly the kind of critical section
 * mutex.h describes.
 *
 * Plain static/global struct -- no dynamic allocation, matching the
 * rest of this kernel.
 */
static Mutex_t uartMutex;

/*
 * Chapter 29 demonstration semaphore -- this is the "UART hardware
 * event" example from semaphore.h/Chapter 29 spec, running for real:
 * a byte arriving on USART2 is an EVENT, not a resource anyone
 * "owns," which is exactly why a semaphore (not another mutex) is
 * the right primitive here.
 *
 * Binary, starting empty (0): commsTask should block until at least
 * one byte has arrived, and should not be able to "pre-consume" a
 * byte that hasn't been received yet.
 *
 * This REPLACES commsTask's old polling loop
 * ("if (rec_data != 0 ...)") with an actual blocking wait -- commsTask
 * no longer burns CPU cycles re-checking rec_data every scheduler
 * timeslice; it is fully off the CPU (BLOCKED, not in any ready list)
 * until USART2_IRQHandler signals it directly.
 */
static Semaphore_t uartRxSem;

#endif /* APP_MODE_APPLICATION */

void SystemInit(void) { }

/*
 * SysTick_Handler does two/three jobs each tick, depending on
 * ACTIVE_APP_MODE (see app_config.h):
 *
 *  1. tick_count++            - always. Tasks/tests use get_tick()
 *                                for their own non-blocking timing.
 *
 *  2. semaphore_test8_tick_hook() - APP_MODE_TEST_PRIORITY only. This
 *                                is semaphore_tests_priority.c's
 *                                stand-in for "a real interrupt
 *                                fired," used to demonstrate Test 8
 *                                (ISR signaling) without needing a
 *                                serial terminal. It is compiled out
 *                                entirely in every other mode.
 *
 *  3. kernel_tick()            - always. Marks PendSV as pending.
 *                                WHICH task that switch lands on is
 *                                governed by priority (Chapter 27)
 *                                and by whether the current task is
 *                                BLOCKED on a mutex or semaphore
 *                                (Chapters 28/29) -- this handler
 *                                doesn't need to know or care about
 *                                either.
 */
#if ACTIVE_APP_MODE == APP_MODE_TEST_PRIORITY
extern void semaphore_test8_tick_hook(void);
#endif

void SysTick_Handler(void) {
    tick_count++;

    /*
     * Test-8 hook may fire in Task/ISR context regardless -- it's
     * designed to work from ISR context and is harmless before
     * kernel_running flips.
     */
#if ACTIVE_APP_MODE == APP_MODE_TEST_PRIORITY
    semaphore_test8_tick_hook();
#endif

    /*
     * Do NOT request a reschedule before restore_context has armed
     * the flag. currentTask is NULL and PendSV would trap.
     */
    if (kernel_running) {
        kernel_tick();
    }
}


void EXTI15_10_IRQHandler(void) {
    if (EXTI_PR & (1 << BTN_PIN)) {
        btn_event = 1;
        EXTI_PR   = (1 << BTN_PIN);
    }
}

#define RX_BUFFER_SIZE 64
static volatile uint8_t  rx_buffer[RX_BUFFER_SIZE];
static volatile uint32_t rx_head = 0;   /* next write position */
static volatile uint32_t rx_tail = 0;   /* next read position  */

/* ISR: enqueue one byte */
void USART2_IRQHandler(void) {
    if (USART2_SR & (1 << 5)) {
        uint8_t byte = USART2_DR;
        uint32_t next = (rx_head + 1) % RX_BUFFER_SIZE;
        if (next != rx_tail) {          /* drop if full */
            rx_buffer[rx_head] = byte;
            rx_head = next;
        }
        semaphore_signal(&uartRxSem);
    }
}

uint32_t get_tick(void) { return tick_count; }

void pll_init(void) {
    RCC_CR      |=  (1 << 16);
    while (!(RCC_CR & (1 << 17)));

    FLASH_ACR   &= ~(0x7);
    FLASH_ACR   |=  (0x2);

    RCC_CFGR    &= ~(0xF << 4);
    RCC_CFGR    |=  (0x4 << 8);
    RCC_CFGR    &= ~(0x7 << 11);

    RCC_CFGR    |=  (1 << 16);
    RCC_CFGR    &= ~(1 << 17);
    RCC_CFGR    &= ~(0xF << 18);

    RCC_CFGR    |=  (0x7 << 18);
    RCC_CR      |=  (1 << 24);
    while (!(RCC_CR & (1 << 25)));
    RCC_CFGR    &= ~(0x3);
    RCC_CFGR    |=  (0x2);
    while ((RCC_CFGR & (0x3 << 2)) != (0x2 << 2));
}

void systick_init(void) {
    SYST_RVR = 71999;
    SYST_CVR = 0;
    SYST_CSR = 0x7;
}

void gpio_init(void) {
    RCC_APB2ENR |= (1 << 0);
    RCC_APB2ENR |= (1 << 2);
    RCC_APB2ENR |= (1 << 4);
    GPIOA_CRL   &= ~(0xF << 20);
    GPIOA_CRL   |=  (0x1 << 20);
    GPIOC_CRH   &= ~(0xF << 20);
    GPIOC_CRH   |=  (0x8 << 20);
    GPIOC_ODR   |=  (1 << BTN_PIN);
}

void exti_init(void) {
    AFIO_EXTICR4 &= ~(0xF << 4);
    AFIO_EXTICR4 |=  (0x2 << 4);
    EXTI_IMR  |= (1 << BTN_PIN);
    EXTI_FTSR |= (1 << BTN_PIN);
    NVIC_ISER1 |= (1 << 8);
}

void usart2_init(void) {
    RCC_APB1ENR |= (1 << 17);
    RCC_APB2ENR |= (1 << 2);
    GPIOA_CRL   &= ~(0xF << 8);
    GPIOA_CRL   |=  (0x9 << 8);
    GPIOA_CRL   &= ~(0xF << 12);
    GPIOA_CRL   |=  (0x4 << 12);
    USART2_CR1  |=  (1 << 13);
    USART2_CR1  &= ~(1 << 12);
    USART2_CR2  &= ~(0x3 << 12);
    USART2_BRR   =   0x139;
    USART2_CR1  |=  (1 << 3);
    USART2_CR1  |=  (1 << 2);
    USART2_CR1  |=  (1 << 5);
    NVIC_ISER1  |=  (1 << 6);
}

void usart2_transmit(uint8_t data) {
    while (!(USART2_SR & (1 << 7)));
    USART2_DR = data;
}

void usart2_send_string(const char *str) {
    while (*str) usart2_transmit((uint8_t)*str++);
}

void usart2_send_number(uint32_t num) {
    if (num == 0) { usart2_transmit('0'); return; }
    char buf[10];
    int i = 0;
    while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    while (i > 0)   { usart2_transmit(buf[--i]); }
}

void usart2_send_signed(int32_t num) {
    if (num < 0) { usart2_transmit('-'); num = -num; }
    usart2_send_number((uint32_t)num);
}

void adc_config(void) {
    RCC_APB2ENR |= (1 << 9);
    RCC_CFGR    &= ~(0x3 << 14);
    RCC_CFGR    |=  (0x2 << 14);
    ADC1_CR2    |=  (1 << 23);
    ADC1_SMPR1  &= ~(0x7 << 18);
    ADC1_SMPR1  |=  (0x7 << 18);
    ADC1_SQR3   &= ~(0x1F << 0);
    ADC1_SQR3   |=  (16 << 0);
    ADC1_SQR1   &= ~(0xF << 20);
    ADC1_CR2    &= ~(1 << 11);
    ADC1_CR2    &= ~(0x7 << 17);
    ADC1_CR2    |=  (0x7 << 17);
    ADC1_CR2    |=  (1 << 20);
    ADC1_CR2    &= ~(1 << 1);
}

void adc_init(void) {
    adc_config();
    ADC1_CR2 |= (1 << 0);
    uint32_t t = get_tick();
    while (get_tick() - t < 1);
    ADC1_CR2 |= (1 << 2);
    while (ADC1_CR2 & (1 << 2));
    /*
     * Runs before kernel_start() -- no scheduler active yet, no
     * other task could possibly interleave here, so no mutex is
     * needed for this one-time startup message.
     */
    usart2_send_string("ADC calibrated\r\n");
}

uint16_t adc_read(void) {
    ADC1_CR2 |= (1 << 22);
    while (!(ADC1_SR & (1 << 1)));
    return ADC1_DR;
}

int32_t temperature_calc(uint16_t raw) {
    uint32_t vsense_mv = ((uint32_t)raw * 3300) / 4096;
    int32_t  temp_x10;
    if (vsense_mv <= 1430) {
        temp_x10 = (int32_t)(((1430 - vsense_mv) * 10) / 43) + 250;
    } else {
        temp_x10 = 250 - (int32_t)(((vsense_mv - 1430) * 10) / 43);
    }
    return temp_x10;
}

// ═══════════════════════════════════════════════════════════════
//  TASK FUNCTIONS — each loops forever internally.
//
//  Unchanged since Chapter 7 in overall shape. Chapter 28 wraps each
//  task's log-line output in uartMutex, since usart2_transmit()
//  ultimately writes to one shared piece of hardware (USART2_DR) any
//  of these tasks could be preempted in the middle of.
//
//  APP_MODE_APPLICATION only -- see app_config.h. The test-mode
//  builds create their own, separate task functions in
//  semaphore_tests_basic.c / semaphore_tests_priority.c instead.
// ═══════════════════════════════════════════════════════════════
#if ACTIVE_APP_MODE == APP_MODE_APPLICATION

void ledTask(void) {
  for (;;) {
    static LedState      current_state  = STATE_LED_OFF;
    static uint32_t      blink_timer    = 0;
    static uint8_t       blink_phase    = 0;
    static DebounceState debounce_state = WAIT_PRESS;
    static uint32_t       debounce_start = 0;
    uint8_t event = 0;

    // ── non-blocking debounce state machine ─────────────────
    switch (debounce_state) {
        case WAIT_PRESS:
            if (btn_event) {
                btn_event = 0;
                debounce_start = get_tick();
                debounce_state = DEBOUNCE;
            }
            break;

        case DEBOUNCE:
            if ((GPIOC_IDR >> BTN_PIN) & 1) {
                // released early — false alarm, go back to watching
                debounce_state = WAIT_PRESS;
            } else if ((get_tick() - debounce_start) >= 20) {
                // held low for a full 20ms — genuine press
                event = 1;
                debounce_state = WAIT_RELEASE;
            }
            break;

        case WAIT_RELEASE:
            if ((GPIOC_IDR >> BTN_PIN) & 1) {
                // button finally let go — ready to detect the next press
                debounce_state = WAIT_PRESS;
            }
            break;
    }

    // ── LED state machine (unchanged logic, now driven by 'event' above) ──
    switch (current_state) {
        case STATE_LED_OFF:
            GPIOA_BSRR = (1 << (LED_PIN + 16));
            if (event) {
                current_state = STATE_LED_ON;
                /*
                 * mutex_lock() here can genuinely block: commsTask
                 * (P1) is higher priority than ledTask (P2) and could
                 * be the one holding uartMutex when this runs. That
                 * is a live, if brief, instance of the priority
                 * inversion documented in mutex.h -- P1 comms output
                 * is never blocked BY this lock, but ledTask's own
                 * progress here can be delayed by lower-priority
                 * mutex ownership elsewhere. Chapter 28 has no
                 * priority inheritance yet, so this is expected.
                 */
                mutex_lock(&uartMutex);
                usart2_send_string("LED State: ");
                usart2_send_number(STATE_LED_ON);
                usart2_send_string("\r\n");
                mutex_unlock(&uartMutex);
            }
            break;

        case STATE_LED_ON:
            GPIOA_BSRR = (1 << LED_PIN);
            if (event) {
                current_state = STATE_LED_BLINK;
                mutex_lock(&uartMutex);
                usart2_send_string("LED State: ");
                usart2_send_number(STATE_LED_BLINK);
                usart2_send_string("\r\n");
                mutex_unlock(&uartMutex);
            }
            break;

        case STATE_LED_BLINK:
            if ((get_tick() - blink_timer) >= 500) {
                blink_timer = get_tick();
                if (blink_phase == 0) {
                    GPIOA_BSRR = (1 << LED_PIN);
                    blink_phase = 1;
                } else {
                    GPIOA_BSRR = (1 << (LED_PIN + 16));
                    blink_phase = 0;
                }
            }
            if (event) {
                blink_phase   = 0;
                current_state = STATE_LED_OFF;
                mutex_lock(&uartMutex);
                usart2_send_string("LED State: ");
                usart2_send_number(STATE_LED_OFF);
                usart2_send_string("\r\n");
                mutex_unlock(&uartMutex);
            }
            break;
    }
  }
}

void tempTask(void) {
  for (;;) {
    static uint32_t temp_timer = 0;

    if ((get_tick() - temp_timer) >= 3000) {
        temp_timer = get_tick();

        uint16_t raw  = adc_read();
        int32_t  temp = temperature_calc(raw);

        mutex_lock(&uartMutex);
        usart2_send_string("Temp: ");
        usart2_send_signed(temp / 10);
        usart2_transmit('.');
        usart2_send_number((uint32_t)(temp % 10));
        usart2_send_string(" C\r\n");
        mutex_unlock(&uartMutex);
    }
  }
}

/* commsTask: drain the buffer */
void commsTask(void) {
    for (;;) {
        semaphore_wait(&uartRxSem);

        while (rx_head != rx_tail) {
            uint8_t b = rx_buffer[rx_tail];
            rx_tail = (rx_tail + 1) % RX_BUFFER_SIZE;
            if (b == 0 || b == 10) continue;   /* filter */

            mutex_lock(&uartMutex);
            usart2_send_string("Received: ");
            usart2_transmit(b);
            usart2_send_string("\r\n");
            mutex_unlock(&uartMutex);
        }
    }
}

#endif /* APP_MODE_APPLICATION */

// ═══════════════════════════════════════════════════════════════
//  MAIN — one-time setup, then hand off to the kernel permanently
// ═══════════════════════════════════════════════════════════════
int main(void) {

	// Diagnostic: toggle PA5 a few times using the HSI (no PLL dependency)
	    RCC_APB2ENR |= (1 << 2);          // GPIOA clock
	    GPIOA_CRL &= ~(0xF << 20);
	    GPIOA_CRL |=  (0x3 << 20);        // PA5 push-pull output 50 MHz
	    for (int i = 0; i < 6; i++) {
	        GPIOA_BSRR = (1 << 5);
	        for (volatile int j = 0; j < 200000; j++);
	        GPIOA_BSRR = (1 << (5 + 16));
	        for (volatile int j = 0; j < 200000; j++);
	    }

    pll_init();
    systick_init();
    gpio_init();
    exti_init();
    usart2_init();
    adc_init();

    usart2_send_string("System started\r\n");

    kernel_init();

    /*
     * Which tasks get created is the ONLY thing that varies between
     * builds -- see app_config.h. Hardware bring-up above runs the
     * same regardless of mode; it's harmless for the test builds to
     * initialize peripherals (GPIO/EXTI/ADC) they don't happen to
     * use.
     */
#if ACTIVE_APP_MODE == APP_MODE_APPLICATION

    mutex_init(&uartMutex);
    semaphore_init_binary(&uartRxSem, 0); /* starts empty: no byte received yet */

    /*
     * Same priority assignment as Chapter 27:
     *   commsTask -> P1 (highest): react to incoming UART bytes promptly.
     *   ledTask   -> P2: responsive to button presses, but not as
     *                urgent as protecting incoming UART data.
     *   tempTask  -> P4: slow background sample, lowest of the three.
     */
    kernel_create_task(commsTask, "Comms", 1);
    kernel_create_task(ledTask,   "LED",   1);
    kernel_create_task(tempTask,  "Temp",  1);

#elif ACTIVE_APP_MODE == APP_MODE_TEST_BASIC

    extern void semaphore_tests_basic_create_tasks(void);
    semaphore_tests_basic_create_tasks();

#elif ACTIVE_APP_MODE == APP_MODE_TEST_PRIORITY

    extern void semaphore_tests_priority_create_tasks(void);
    semaphore_tests_priority_create_tasks();

#endif

    kernel_start();   // never returns

    while (1);
}
