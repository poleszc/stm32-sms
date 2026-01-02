#include "cyclic_buffer.h"
#include <ctype.h>
#include <gpio.h>
#include <lcd.h>
#include <stdbool.h>
#include <stm32.h>
#include <string.h>

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

#define TICK_MS 10
// After 800ms the letter is changes if the same key is pressed.
#define MULTI_TAP_TIMEOUT (800 / TICK_MS)
// After 500ms a key hold is considered a hold.
#define HOLD_DELAY (500 / TICK_MS)
// After 500ms if a key hold is recognized we repeat the action again
#define REPEAT_RATE (800 / TICK_MS)
#define LCD_ROWS 5
#define LCD_COLUMNS 9
#define KEYBOARD_ROWS 4
#define KEYBOARD_COLUMNS 4

static const int column_pins[] = {0, 1, 2, 3}; // PC0, PC1, PC2, PC3
static const int row_pins[] = {6, 7, 8, 9};    // PC6, PC7, PC8, PC9
static const char *small_key_map[4][4] = {
    {".,?!1", "abc2", "def3", NULL},  // A -> Shift
    {"ghi4", "jkl5", "mno6", NULL},   // B -> Backspace
    {"pqrs7", "tuv8", "wxyz9", NULL}, // C -> Clear
    {"*", " 0", "#", NULL},           // D -> Enter/Newline
};

static const char keymap[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

// Editor state
typedef struct {
    int cursor_x;      // Cursor position on screen in column
    int cursor_y;      // Cursor position on screen in raw
    bool is_uppercase; // Is shift pressed.
    char last_raw_key;
    int multi_tap_idx;
    int last_confirmed_idx; // For repeating action
    int timeout_counter;
    bool waits_for_confirmation;
} SMS_Editor_State_t;

static SMS_Editor_State_t editor = {0, 0, false, '\0', 0, 0, 0, false};
// ==================== LCD ====================
static void editor_clear_screen(void) {
    LCDclear();
    editor.cursor_x = 0;
    editor.cursor_y = 0;
    LCDgoto(0, 0);
}

static void editor_new_line(void) {
    editor.cursor_x = 0;
    editor.cursor_y++;
    if (editor.cursor_y > LCD_ROWS)
        editor.cursor_y = 0;
    LCDgoto(editor.cursor_x, editor.cursor_y);
}

static void editor_put_char(char c, bool is_preview) {
    if (isalpha((unsigned char)c)) {
        c = editor.is_uppercase ? toupper((unsigned char)c) : c;
    }

    LCDputchar(c);

    if (is_preview) {
        // If a key is a preview (not accepted yet) we go back with the cursor.
        LCDgoto(editor.cursor_y, editor.cursor_x);
    }
}

static void handle_sms_logic(char key, bool is_repeat) {
    if (key == 'A') { // CapsLock
        if (!is_repeat)
            editor.is_uppercase = !editor.is_uppercase;
        return;
    }

    if (key == 'B') {
        // If we we're in the middle of multi-tap, we just clear the current place
        if (editor.waits_for_confirmation) {
            editor.waits_for_confirmation = false;
            editor.last_raw_key = '\0';

            LCDgoto(editor.cursor_y, editor.cursor_x);
            LCDputchar(' ');
            LCDgoto(editor.cursor_y, editor.cursor_x);
        } else {
            if (editor.cursor_x > 0) { // This line not empty.
                editor.cursor_x--;
            } else if (editor.cursor_y > 0) { // This line empty, take one step higher.
                editor.cursor_y--;
                editor.cursor_x = LCD_COLUMNS - 1;
            }
            LCDgoto(editor.cursor_y, editor.cursor_x);
            LCDputchar(' ');
            LCDgoto(editor.cursor_y, editor.cursor_x);
        }
        return;
    }

    // Don't repeat newlines and clears.
    if (is_repeat && (key == 'C' || key == 'D'))
        return;

    if (key == 'C') {
        editor_clear_screen();
        editor.waits_for_confirmation = false;
        editor.last_raw_key = '\0';
        editor.timeout_counter = 0;
        return;
    }

    if (key == 'D') {
        // If there was a multi-tap in progress we accept it and go to the new line.
        if (editor.waits_for_confirmation) {
            editor.cursor_x++;
        }
        editor.waits_for_confirmation = false;
        editor.last_raw_key = '\0';
        editor_new_line();
        return;
    }

    // Find the index in the map.
    int row = -1, col = -1;
    for (int i = 0; i < KEYBOARD_ROWS; i++) {
        for (int j = 0; j < KEYBOARD_COLUMNS; j++) {
            if (keymap[i][j] == key) {
                row = i;
                col = j;
                break;
            }
        }
    }

    // Special keys should be handled above.
    const char *chars = small_key_map[row][col];
    if (chars == NULL)
        return;
    int len = strlen(chars);

    if (is_repeat) {
        // If we started holding the same key as before after a break we use the saved confirmed
        // index.
        if (key == editor.last_raw_key && editor.multi_tap_idx == 0) {
            editor.multi_tap_idx = editor.last_confirmed_idx;
        }

        if (editor.waits_for_confirmation) {
            LCDgoto(editor.cursor_y, editor.cursor_x);
            editor_put_char(chars[editor.multi_tap_idx], false);

            editor.cursor_x++;
            if (editor.cursor_x >= LCD_COLUMNS)
                editor_new_line();
            editor.waits_for_confirmation = false;
        } else {
            LCDgoto(editor.cursor_y, editor.cursor_x);
            editor_put_char(chars[editor.multi_tap_idx], false);
            editor.cursor_x++;
            if (editor.cursor_x >= LCD_COLUMNS)
                editor_new_line();
        }

    } else {
        if (key == editor.last_raw_key && editor.waits_for_confirmation) {
            // Same key as before. Reset the timeout counter and loop to the next char.
            editor.multi_tap_idx = (editor.multi_tap_idx + 1) % len;
        } else {
            if (key != editor.last_raw_key) {
                editor.last_confirmed_idx = 0;
            }

            // New key. Put the previous one on the screen if it wasn't confirmed.
            if (editor.waits_for_confirmation) {
                editor.cursor_x++;
                if (editor.cursor_x >= LCD_COLUMNS) {
                    editor_new_line();
                }
            }
            editor.last_raw_key = key;
            editor.multi_tap_idx = 0;
            editor.waits_for_confirmation = true;
        }
        // Show preview.
        editor.timeout_counter = 0;
        LCDgoto(editor.cursor_y, editor.cursor_x);
        editor_put_char(chars[editor.multi_tap_idx], true);
    }
}

// ==================== SETUP ====================
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

// ==================== KEYBOARD ====================
/**
 * @brief Scans the keyboard state. Returns the char of the pressed button or '\0' otherwise.
 */
static char scanKeyboard(void) {
    for (int col = 0; col < 4; col++) {
        // Low state on column.
        GPIOC->BSRR = (1 << (column_pins[col] + 16));

        // Give the microcontroler some time to process the state change on the column. First row
        // doesn't work if this isn't here.
        __NOP();

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
        EXTI->IMR &= ~ROW_PINS;
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
    static int hold_timer = 0;

    // Sprawdzamy czy to nasze przerwanie.
    // TODO: Czy tak trzeba?
    if (TIM3->SR & TIM_SR_UIF) {
        // Reset the interrupt flag.
        TIM3->SR = ~TIM_SR_UIF;

        char current_key = scanKeyboard();

        if (current_key != '\0') {
            // New key, put it on the screen.
            if (current_key != last_key) {
                handle_sms_logic(current_key, false);
                last_key = current_key;
                hold_timer = 0;
            } else { // Same key as before
                hold_timer++;
                if (hold_timer >= HOLD_DELAY) {
                    // If the key is held, we repeat the action once in REPEAT_RATE.
                    if ((hold_timer - HOLD_DELAY) % REPEAT_RATE == 0) {
                        handle_sms_logic(current_key, true);
                    }
                }
            }
        } else { // If nothing is pressed.
            // Reset last key.
            last_key = '\0';
            hold_timer = 0;

            if (editor.waits_for_confirmation) {
                editor.timeout_counter++;
                // Timeout exceeded accept the key. It's already on the screen so we just move the
                // cursor.
                if (editor.timeout_counter >= MULTI_TAP_TIMEOUT) {
                    editor.last_confirmed_idx = editor.multi_tap_idx;
                    editor.waits_for_confirmation = false; // Key is confirmed.
                    // editor.last_raw_key = '\0';
                    editor.cursor_x++;
                    if (editor.cursor_x >= LCD_COLUMNS) {
                        editor_new_line();
                    } else {
                        LCDgoto(editor.cursor_y, editor.cursor_x);
                    }
                }

                // If no keys wait for confirmation we turn of the timer.
                if (!editor.waits_for_confirmation) {
                    // Stop the timer.
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
    }
}

int main(void) {
    LCDconfigure();
    setupKeyboardInterrupts();
    setupBuffers();

    for (;;) {
        __WFI();
    }
}