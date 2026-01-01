#include "cyclic_buffer.h"
#include <gpio.h>
#include <lcd.h>
#include <stdbool.h>
#include <stm32.h>

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

// ==================== GLOBAL STATE ====================
#define BUFFER_SIZE 128
static char internal_out_buffer[BUFFER_SIZE];
static CyclicBuffer_t out_buffer_struct;
static CyclicBuffer_t *out_buffer = &out_buffer_struct;

static const int column_pins[] = {0, 1, 2, 3}; // PC0, PC1, PC2, PC3
static const int row_pins[] = {6, 7, 8, 9};    // PC6, PC7, PC8, PC9
static const char keymap[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

// ==================== UTILS ====================
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

// ==================== SETUP ====================
static void setupUart(void) {
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

static void setupKeyboardInterrupts(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN; // TODO: Why?

    // Configure the counter. Don't turn on yet.
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    TIM3->PSC = 16000 - 1;      // 1ms tick
    TIM3->ARR = 10 - 1;         // 10ms period
    TIM3->DIER |= TIM_DIER_UIE; // Interrupt enable.

    // Setup columns.
    // TODO: Wyjście przeciwsobne - co to
    // Bez rezystorów pociągających - GPIO_PuPd_NOPULL
    for (int i = 0; i < 4; i++) {
        GPIOoutConfigure(GPIOC, column_pins[i], GPIO_OType_PP, GPIO_Low_Speed, GPIO_PuPd_NOPULL);
        GPIOC->BSRR = (1 << (column_pins[i] + 16)); // Set low state on columns.
    }

    // Setup rows.
    // Rezystory podciągające - Pull-up
    // Zgłasza przerwanie przy zboczu opadającym - EXTI_Mode_Interrupt, EXTI_Trigger_Falling.
    // TODO: Dlaczego przy opadającym?
    for (int i = 0; i < 4; i++) {
        GPIOinConfigure(GPIOC, row_pins[i], GPIO_PuPd_UP, EXTI_Mode_Interrupt,
                        EXTI_Trigger_Falling);
    }

    // Uruchomienie przerwań dla wierszy
    NVIC_EnableIRQ(EXTI9_5_IRQn);
    NVIC_EnableIRQ(TIM3_IRQn);
}

static void setupBuffers(void) { cycBufInit(out_buffer, internal_out_buffer, BUFFER_SIZE); }

// ==================== UART ====================
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

// ==================== KEYBOARD ====================
/**
 * @brief Scans the keyboard state. Returns the char of the pressed button or '\0' otherwise.
 */
static char scanKeyboard(void) {
    for (int col = 0; col < 4; col++) {
        // Low state on column.
        GPIOC->BSRR = (1 << (column_pins[col] + 16));

        // TODO: Why
        for (volatile int i = 0; i < 100; i++)
            ;

        for (int row = 0; row < 4; row++) {
            // If row pin is in inactive state, then this key is pressed.
            if (!(GPIOC->IDR & (1 << row_pins[row]))) {
                // TODO: Decide if the key if held or pressed.
                // Before returning, put all columns in the high state again.
                for (int col2 = 0; col2 < 4; col2++)
                    GPIOC->BSRR = (1 << column_pins[col2]);
                return keymap[row][col];
            }
        }

        // Set column in active state.
        GPIOC->BSRR = (1 << column_pins[col]);
    }
    return '\0';
}

#define ROW_PINS ((1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))

// ==================== INTERRUPTS ====================
void EXTI9_5_IRQHandler(void) {
    // Check if the interrupt is on 6-9 pins.
    if (EXTI->PR & ROW_PINS) {
        // Deactivate row interrupts.
        EXTI->IMR &= !ROW_PINS;
        // Wyzeruj znaczniki przerwań wierszy.
        EXTI->PR = ROW_PINS;

        // Active state on all columns.
        for (int col = 0; col < 4; col++) {
            GPIOC->BSRR = (1 << column_pins[col]);
        }

        // Wyzeruj rejestr CNT licznika.
        TIM3->CNT = 0;
        // Uruchom licznik.
        TIM3->CR1 |= TIM_CR1_CEN;
    }
}

void TIM3_IRQHandler(void) {
    static char last_key = '\0';

    // Sprawdzamy czy to nasze przerwanie.
    // TODO: Czy tak trzeba?
    if (TIM3->SR & TIM_SR_UIF) {
        // Wyzeruj znacznik przerwania
        TIM3->SR = ~TIM_SR_UIF;

        char current_key = scanKeyboard();

        if (current_key != '\0') {
            // If a different key then before was pressed, put it on the screen.
            if (current_key != last_key) {
                char msg[] = "Key: X";
                msg[5] = current_key;
                printLine(msg);
                LCDputcharWrap(current_key);
                last_key = current_key;
            }
        } else { // If nothing is pressed.
            // Reset last key.
            last_key = '\0';
            // Stop the counter.
            TIM3->CR1 &= ~TIM_CR1_CEN;
            // Low state on all columns.
            for (int col = 0; col < 4; col++) {
                GPIOC->BSRR = (1 << (column_pins[col] + 16));
            }
            // Reset the EXTI interrupts.
            EXTI->PR = ROW_PINS;
            // Activate the interrupts.
            EXTI->IMR |= ROW_PINS;
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

int main(void) {
    LCDconfigure();
    setupKeyboardInterrupts();
    setupBuffers();
    setupUart();

    printLine("SMS Init");
    lcdWriteLine("Press key:");

    for (;;) {
        uartWrite();
        // __WFI();
    }
}