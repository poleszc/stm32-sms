#include "cyclic_buffer.h"
#include <gpio.h>
#include <lcd.h>
#include <stdbool.h>
#include <stm32.h>

// ===== LEDS CONFIG =====
// TODO: Remove those
#define RED_LED_GPIO GPIOA
#define GREEN_LED_GPIO GPIOA
#define BLUE_LED_GPIO GPIOB
#define GREEN2_LED_GPIO GPIOA
#define RED_LED_PIN 6
#define GREEN_LED_PIN 7
#define BLUE_LED_PIN 0
#define GREEN2_LED_PIN 5

#define RedLEDon() RED_LED_GPIO->BSRR = 1 << (RED_LED_PIN + 16)
#define RedLEDoff() RED_LED_GPIO->BSRR = 1 << RED_LED_PIN
#define BlueLEDon() BLUE_LED_GPIO->BSRR = 1 << (BLUE_LED_PIN + 16)
#define BlueLEDoff() BLUE_LED_GPIO->BSRR = 1 << (BLUE_LED_PIN)
#define GreenBigLEDon() GREEN_LED_GPIO->BSRR = 1 << (GREEN_LED_PIN + 16)
#define GreenBigLEDoff() GREEN_LED_GPIO->BSRR = 1 << (GREEN_LED_PIN)
#define GreenSmallLEDon() GREEN2_LED_GPIO->BSRR = 1 << GREEN2_LED_PIN
#define GreenSmallLEDoff() GREEN2_LED_GPIO->BSRR = 1 << (GREEN2_LED_PIN + 16)

// ===== USART CONFIG =====
#define USART_Enable USART_CR1_UE
#define USART_Mode_Rx_Tx (USART_CR1_RE | USART_CR1_TE)

#define USART_WordLength_8b 0x0000
#define USART_WordLength_9b USART_CR1_M
#define USART_Parity_No 0x0000
#define USART_Parity_Even USART_CR1_PCE
#define USART_Parity_Odd (USART_CR1_PCE | USART_CR1_PS)
#define USART_StopBits_1 0x0000
#define USART_StopBits_0_5 0x1000
#define USART_StopBits_2 0x2000
#define USART_StopBits_1_5 0x3000

#define USART_FlowControl_None 0x0000
#define USART_FlowControl_RTS USART_CR3_RTSE
#define USART_FlowControl_CTS USART_CR3_CTSE

#define HSI_HZ 16000000U
#define PCLK1_HZ HSI_HZ
#define BAUD 9600U

// ===== BUTTON CONFIG =====
#define USER_BTN_GPIO GPIOC
#define USER_BTN_PIN 13
#define USER_BTN_ON_STATE 0

#define MODE_BTN_GPIO GPIOA
#define MODE_BTN_PIN 0
#define MODE_BTN_ON_STATE 1

#define UP_BTN_GPIO GPIOB
#define UP_BTN_PIN 5
#define UP_BTN_ON_STATE 0
#define DOWN_BTN_GPIO GPIOB
#define DOWN_BTN_PIN 6
#define DOWN_BTN_ON_STATE 0
#define RIGHT_BTN_GPIO GPIOB
#define RIGHT_BTN_PIN 4
#define RIGHT_BTN_ON_STATE 0
#define LEFT_BTN_GPIO GPIOB
#define LEFT_BTN_PIN 3
#define LEFT_BTN_ON_STATE 0
#define FIRE_BTN_GPIO GPIOB
#define FIRE_BTN_PIN 10
#define FIRE_BTN_ON_STATE 0

// ===== GLOBAL STATE =====

#define BUFFER_SIZE 128
static char internal_out_buffer[BUFFER_SIZE];
static CyclicBuffer_t out_buffer_struct;

static CyclicBuffer_t *out_buffer = &out_buffer_struct;

static int strLen(const char *line) {
    int i = 0;
    while (line[i] != '\0')
        i++;
    return i;
}

static void printLine(const char *line) {
    if (strLen(line) + 2 > cycBufCapacity(out_buffer)) {
        return;
    }
    int i = 0;
    while (line[i] != '\0') {
        cycBufWrite(out_buffer, line[i++]);
    }
    cycBufWrite(out_buffer, '\r');
    cycBufWrite(out_buffer, '\n');
}

// ===== SETUP =====
static void setupLeds(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
    __NOP();
    RedLEDoff();
    GreenBigLEDoff();
    BlueLEDoff();
    GreenSmallLEDoff();

    GPIOoutConfigure(RED_LED_GPIO, RED_LED_PIN, GPIO_OType_PP, GPIO_Low_Speed, GPIO_PuPd_NOPULL);
    GPIOoutConfigure(BLUE_LED_GPIO, BLUE_LED_PIN, GPIO_OType_PP, GPIO_Low_Speed, GPIO_PuPd_NOPULL);
    GPIOoutConfigure(GREEN_LED_GPIO, GREEN_LED_PIN, GPIO_OType_PP, GPIO_Low_Speed,
                     GPIO_PuPd_NOPULL);
    GPIOoutConfigure(GREEN2_LED_GPIO, GREEN2_LED_PIN, GPIO_OType_PP, GPIO_Low_Speed,
                     GPIO_PuPd_NOPULL);
}

static void setupUsart(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;

    GPIOafConfigure(GPIOA, 2, GPIO_OType_PP, GPIO_Fast_Speed, GPIO_PuPd_NOPULL, GPIO_AF_USART2);
    GPIOafConfigure(GPIOA, 3, GPIO_OType_PP, GPIO_Fast_Speed, GPIO_PuPd_UP, GPIO_AF_USART2);

    USART2->CR1 = USART_Mode_Rx_Tx | USART_WordLength_8b | USART_Parity_No;
    USART2->CR2 = USART_StopBits_1;
    USART2->CR3 = USART_FlowControl_None;
    USART2->BRR = (PCLK1_HZ + (BAUD / 2U)) / BAUD;
    USART2->CR1 |= USART_Enable;
}

static void setupBuffers(void) { cycBufInit(out_buffer, internal_out_buffer, BUFFER_SIZE); }

/**
 * @brief Read one char from the `out_buffer` and send it to UART.
 */
static void uartWrite(void) {
    if (!cycBufEmpty(out_buffer) && (USART2->SR & USART_SR_TXE)) {
        char val = '\0';
        cycBufRead(out_buffer, &val);
        USART2->DR = val;
    }
}

// ==== KEYBOARD =====
static const int column_pins[] = {0, 1, 2, 3}; // PC0, PC1, PC2, PC3
static const int row_pins[] = {6, 7, 8, 9};    // PC6, PC7, PC8, PC9
static const char keymap[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};
static const char *big_key_map[] = {" 0",
                                  ".,?1!1",
                                  "ABC2"
                                  "DEF3",
                                  "GHI4",
                                  "JKL5",
                                  "MNO6",
                                  "PQRS7",
                                  "TUV8",
                                  "WXYZ9"};

#define MULTI_TAP_TIMEOUT 1000 // 1 second (in miliseconds)

volatile uint32_t system_mills = 0;
uint32_t mills() { return system_mills; }

static void setupKeyboard(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;

    // Setup columns.
    for (int i = 0; i < 4; i++) {
        GPIOoutConfigure(GPIOC, column_pins[i], GPIO_OType_PP, GPIO_Low_Speed, GPIO_PuPd_NOPULL);
        GPIOC->BSRR = (1 << column_pins[i]);
    }

    // Setup rows.
    for (int i = 0; i < 4; i++) {
        GPIOinConfigure(GPIOC, row_pins[i], GPIO_PuPd_UP, EXTI_Mode_Disable, EXTI_Trigger_Falling);
    }
}

static char scanKeyboard(void) {
    for (int col = 0; col < 4; col++) {
        GPIOC->BSRR = (1 << (column_pins[col] + 16)); // Low state on column.

        for (volatile int i = 0; i < 100; i++)
            ;

        for (int row = 0; row < 4; row++) {
            // If row pin is in inactive state, then this key is pressed.
            if (!((GPIOC->IDR >> row_pins[row]) & 1)) {
                // Active state on column again.
                GPIOC->BSRR = (1 << column_pins[col]);
                return keymap[row][col];
            }
        }

        // Deactivate the column -> set it in active state.
        GPIOC->BSRR = (1 << column_pins[col]);
    }
    return '\0';
}

static void checkKeyboardState(void) {
    static char last_key_pressed = '\0';
    char current_key = scanKeyboard();

    if (current_key != '\0' && current_key != last_key_pressed) {
        char msg[] = "Key: X";
        msg[5] = current_key;
        printLine(msg);

        if (current_key == '*') {
            LCDclear();
        } else if (current_key == '#') {
            LCDputchar('\n');
        } else {
            LCDputcharWrap(current_key);
        }
    }

    last_key_pressed = current_key;
}

static char last_key_pressed = '\0';
static int tap_count = 0;
static uint32_t last_key_time = 0;
int cursor_pos = 0;

static void processSmsEditor(void) {
    if (last_key_pressed != '\0' && (mills() - last_key_time > MULTI_TAP_TIMEOUT)) {
        cursor_pos++;
        last_key_pressed = '\0';
        tap_count = 0;
    }

    char current_key = scanKeyboard();

    if (current_key != '\0') {
        last_key_time = mills();
		// TODO: Remove
        char msg[] = "Key: X";
        msg[5] = current_key;
        printLine(msg);

        if (current_key >= '0' && current_key <= '9') {
            int key_index = current_key - '0';
            const char *key_chars = big_key_map[key_index];
            // TODO: Make use of the stdlib strlen.
            int key_chars_len = strLen(key_chars);

            if (current_key == last_key_pressed) { // Same key
                tap_count++;
                char char_to_display = key_chars[tap_count % key_chars_len];
                LCDgoto(cursor_pos / 9, cursor_pos % 9);
                LCDputchar(char_to_display);
            } else { // Different key.
                if (last_key_pressed != '\0') {
                    cursor_pos++;
                }
                last_key_pressed = current_key;
                tap_count = 0;
                char char_to_display = key_chars[0];
                LCDgoto(cursor_pos / 9, cursor_pos % 9);
                LCDputchar(char_to_display);
            }
        } else if (current_key == 'C') {
            last_key_pressed = '\0';
            tap_count = 0;
            if (cursor_pos > 0) {
                cursor_pos--;
                LCDgoto(cursor_pos / 9, cursor_pos % 9);
                LCDputchar(' ');
                LCDgoto(cursor_pos / 9, cursor_pos % 9);
            }
        } else if (current_key == '*')  {
			LCDclear();
			last_key_pressed = '\0';
			cursor_pos = 0;
			tap_count = 0;
		}
    }
}

#define LCD_CAPACITY 45
static void lcdWriteLine(const char *line) {
    if (strLen(line) > LCD_CAPACITY - 1) {
        return;
    }
    int i = 0;
    while (line[i] != '\0') {
        LCDputcharWrap(line[i]);
        i++;
    }
    LCDputcharWrap('\n');
}

void SysTick_Handler(void) { system_mills++; }

int main(void) {
    setupLeds();
    setupUsart();
    setupBuffers();
    setupKeyboard();
    LCDconfigure();

    printLine("SMS Init");
    lcdWriteLine("Press key:");

    for (;;) {
		// processSmsEditor();
        checkKeyboardState();
        uartWrite();
    }
}