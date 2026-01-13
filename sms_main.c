#include <ctype.h>
#include <gpio.h>
#include <lcd.h>
#include <stdbool.h>
#include <stm32.h>
#include <string.h>

// ==================== CONFIG ====================
// Application.
#define TICK_MS 10
// Time after which the letter is confirmed.
#define MULTI_TAP_TIMEOUT (800 / TICK_MS) // 800ms
// Time after a key hold is considered a hold.
#define HOLD_DELAY (800 / TICK_MS) // 800ms
// Time after a hold action is repeated.
#define REPEAT_RATE (800 / TICK_MS) // 800ms

#define LCD_ROWS 5
#define LCD_COLUMNS 9
#define KEYBOARD_ROWS 4
#define KEYBOARD_COLUMNS 4

// Hardware.
#define KEYBOARD_GPIO GPIOC
#define ROW_PINS_MASK ((1 << 6) | (1 << 7) | (1 << 8) | (1 << 9))
static const int keyboard_column_pins[] = {0, 1, 2, 3}; // PC0, PC1, PC2, PC3
static const int keyboard_row_pins[] = {6, 7, 8, 9};    // PC6, PC7, PC8, PC9

// ==================== GLOBAL STATE ====================
static const char key_chars[KEYBOARD_ROWS][KEYBOARD_COLUMNS] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

static const char *multi_tap_map[KEYBOARD_ROWS][KEYBOARD_COLUMNS] = {
    {".,?!1", "abc2", "def3", NULL},  // A -> Shift
    {"ghi4", "jkl5", "mno6", NULL},   // B -> Backspace
    {"pqrs7", "tuv8", "wxyz9", NULL}, // C -> Clear
    {"*", " 0", "#", NULL},           // D -> Enter/Newline
};

typedef struct {
    int cursor_x;                // Cursor position on screen in column
    int cursor_y;                // Cursor position on screen in raw
    int line_lengths[LCD_ROWS];  // How many characters are in each line. Used when doing backspace.
    bool is_uppercase;           // Is caps-lock on.
    char last_raw_key;           // Last pressed key.
    int multi_tap_idx;           // Index of the char in the `multi_tap_map`.
    int last_confirmed_idx;      // Last confirmed index if the same key was held after a pause.
    int timeout_counter;         // Counter for deciding when the character was accepted.
    bool waits_for_confirmation; // Wether the currently pressed key waits for a timeout to be
                                 // confirmed.
} SMS_Editor_State_t;

static SMS_Editor_State_t editor = {0};

// ==================== KEYBOARD ====================
static void keyboard_set_columns_low(void) {
    for (int i = 0; i < KEYBOARD_COLUMNS; i++)
        KEYBOARD_GPIO->BSRR = (1 << (keyboard_column_pins[i] + 16));
}

static void keyboard_set_columns_high(void) {
    for (int i = 0; i < KEYBOARD_COLUMNS; i++)
        KEYBOARD_GPIO->BSRR = (1 << (keyboard_column_pins[i]));
}

static void keyboard_activate_exti(void) {
    EXTI->PR = ROW_PINS_MASK;
    EXTI->IMR |= ROW_PINS_MASK;
}

/**
 * @brief Scans the keyboard state. Returns the char of the pressed button or '\0' if nothing was
 * pressed.
 */
static char keyboard_scan(void) {
    for (int col = 0; col < KEYBOARD_COLUMNS; col++) {
        GPIOC->BSRR = (1 << (keyboard_column_pins[col] + 16)); // Set column low.

        // Give the microcontroler some time to process the state change on the column. First row
        // doesn't work if this isn't here.
        __NOP();

        for (int row = 0; row < KEYBOARD_ROWS; row++) {
            // If row pin is in inactive state, then this key is pressed.
            if (!(GPIOC->IDR & (1 << keyboard_row_pins[row]))) {
                // Before returning, put all columns in the high state again.
                keyboard_set_columns_high();
                return key_chars[row][col];
            }
        }
        GPIOC->BSRR = (1 << keyboard_column_pins[col]); // Set column high.
    }
    return '\0';
}

// ==================== LCD ====================
/**
 * @brief Go to the (x, y) position on screen.
 * Update the internal editor state with the new position.
 */
static void editor_goto_pos(int x, int y) {
    editor.cursor_x = x;
    editor.cursor_y = y;
    LCDgoto(y, x);
}

/**
 * @brief Move to the next free character space on the LCD. Move to the next line if current line is
 * filled. Update the internal editor state with the new position.
 */
static void editor_move_next(void) {
    editor.line_lengths[editor.cursor_y] = editor.cursor_x + 1;
    editor.cursor_x++;
    if (editor.cursor_x >= LCD_COLUMNS) {
        editor.cursor_x = 0;
        editor.cursor_y = (editor.cursor_y + 1) % LCD_ROWS;
        editor.line_lengths[editor.cursor_y] = 0;
    }
    LCDgoto(editor.cursor_y, editor.cursor_x);
}

/**
 * @brief Clear the LCD screen and move to (0, 0).
 */
static void editor_clear_screen(void) {
    LCDclear();
    for (int i = 0; i < LCD_ROWS; i++)
        editor.line_lengths[i] = 0;
    editor_goto_pos(0, 0);
    editor.waits_for_confirmation = false;
    editor.last_raw_key = '\0';
}

/**
 * @brief Put a character on the current position on the LCD.
 * @param c char to put on the screen.
 * @param is_preview don't change cursor position after the char was inserted.
 */
static void editor_put_char(char c, bool is_preview) {
    if (isalpha((unsigned char)c)) {
        c = editor.is_uppercase ? toupper((unsigned char)c) : c;
    }

    LCDputchar(c);

    if (is_preview) {
        // If a key is a preview (not accepted yet) we move the cursor back to the previous
        // position.
        LCDgoto(editor.cursor_y, editor.cursor_x);
    }
}

/**
 * @brief Remove the last typed character from the LCD screen.
 */
static void editor_backspace(void) {
    if (editor.waits_for_confirmation) {
        // If we we're in the middle of multi-tap, we just clear the current place
        editor.waits_for_confirmation = false;
        editor.last_raw_key = '\0';
    } else {
        // If we last char was accepted, we take a step back end remove it.
        if (editor.cursor_x > 0) { // This line not empty.
            editor.cursor_x--;
            editor.line_lengths[editor.cursor_y]--;
        } else if (editor.cursor_y > 0) { // This line empty, take one step higher.
            editor.cursor_y--;
            // Go to the last written char in the previous line.
            editor.cursor_x = editor.line_lengths[editor.cursor_y];
            if (editor.cursor_x > 0) {
                editor.cursor_x--;
                editor.line_lengths[editor.cursor_y]--;
            }
        } else { // Already in (0, 0).
            return;
        }
    }
    LCDgoto(editor.cursor_y, editor.cursor_x);
    LCDputchar(' ');
    LCDgoto(editor.cursor_y, editor.cursor_x);
}

/**
 * @brief Confirm the pending character. This is called after a timeout was exceeded and the typed
 * char was accepted. Moves the the cursor to the next free spot.
 */
static void editor_confirm_char(void) {
    if (editor.waits_for_confirmation) {
        editor.last_confirmed_idx = editor.multi_tap_idx;
        editor.waits_for_confirmation = false;
        editor_move_next();
    }
}

/**
 * @brief Handles the logic for a key press.
 * @param key The key that was pressed.
 * @param is_held_down Was this key held down, which means a repeat action.
 */
static void handle_sms_logic(char key, bool is_held_down) {
    // Action keys.
    if (key == 'A') { // CapsLock
        if (!is_held_down)
            editor.is_uppercase = !editor.is_uppercase;
        return;
    }
    if (key == 'B') {
        editor_backspace();
        return;
    }
    if (key == 'C') {
        if (!is_held_down)
            editor_clear_screen();
        return;
    }

    if (key == 'D') {
        if (is_held_down) // Don't repeat new lines.
            return;
        editor_confirm_char(); // Confirm current char before new line.
        int next_y = (editor.cursor_y + 1) % LCD_ROWS;
        editor.line_lengths[next_y] = 0;
        editor_goto_pos(0, next_y);
        editor.last_raw_key = '\0';
        return;
    }

    // Find the index in the map.
    int row = -1, col = -1;
    for (int i = 0; i < KEYBOARD_ROWS; i++) {
        for (int j = 0; j < KEYBOARD_COLUMNS; j++) {
            if (key_chars[i][j] == key) {
                row = i;
                col = j;
                break;
            }
        }
    }

    // Special keys should be handled above.
    const char *chars = multi_tap_map[row][col];
    if (chars == NULL)
        return;
    int len = strlen(chars);

    if (is_held_down) {
        // If we started holding the same key as before after a break we use the saved confirmed
        // index.
        if (key == editor.last_raw_key) {
            if (editor.waits_for_confirmation) {
                if (editor.multi_tap_idx == 0) {
                    editor.multi_tap_idx = editor.last_confirmed_idx;
                }

                editor.last_confirmed_idx = editor.multi_tap_idx;
                editor.waits_for_confirmation = false;
            }
            editor_put_char(chars[editor.last_confirmed_idx], false);
            editor_move_next();
        }
    } else {
        if (key == editor.last_raw_key && editor.waits_for_confirmation) {
            // Same key as before. Reset the timeout counter and loop to the next char.
            editor.multi_tap_idx = (editor.multi_tap_idx + 1) % len;
        } else {
            // Different key pressed. Confirm the current one and start new.
            editor_confirm_char();
            editor.last_raw_key = key;
            editor.multi_tap_idx = 0;
            editor.waits_for_confirmation = true;
        }
        // Show preview.
        editor.timeout_counter = 0;
        editor_put_char(chars[editor.multi_tap_idx], true);
    }
}

// ==================== INTERRUPTS ====================
void EXTI9_5_IRQHandler(void) {
    // Check if the interrupt is on 6-9 pins.
    if (EXTI->PR & ROW_PINS_MASK) {
        // Deactivate row interrupts to handle debouncing.
        EXTI->IMR &= ~ROW_PINS_MASK;
        // Reset interrupt flags.
        EXTI->PR = ROW_PINS_MASK;
        // Set all columns in high state.
        keyboard_set_columns_high();
        // Start the timer.
        TIM3->CNT = 0;
        TIM3->CR1 |= TIM_CR1_CEN;
    }
}

void TIM3_IRQHandler(void) {
    static char last_key = '\0';
    static int hold_timer = 0;

    if (TIM3->SR & TIM_SR_UIF) {
        // Reset the interrupt flag.
        TIM3->SR = ~TIM_SR_UIF;

        char current_key = keyboard_scan();

        if (current_key != '\0') {
            // New key, put it on the screen.
            if (current_key != last_key) {
                handle_sms_logic(current_key, false);
                last_key = current_key;
                hold_timer = 0;
            } else { // Same key as before
                hold_timer++;
                // If the key is held, we repeat the action once in REPEAT_RATE.
                if (hold_timer >= HOLD_DELAY && ((hold_timer - HOLD_DELAY) % REPEAT_RATE == 0)) {
                    handle_sms_logic(current_key, true);
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
                    editor_confirm_char();
                }
            } else {
                // Stop the timer and enable EXTI.
                TIM3->CR1 &= ~TIM_CR1_CEN;
                // Low state on all columns.
                keyboard_set_columns_low();
                // Turn EXTI back on.
                keyboard_activate_exti();
            }
        }
    }
}

// ==================== SETUP ====================
static void setupKeyboardInterrupts(void) {
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    // Configure the counter. Don't turn on yet.
    RCC->APB1ENR |= RCC_APB1ENR_TIM3EN;
    TIM3->PSC = 16000 - 1;      // 1ms tick
    TIM3->ARR = TICK_MS - 1;    // 10ms period
    TIM3->DIER |= TIM_DIER_UIE; // Interrupt enable.

    // Setup keyboard columns: Push-Pull, No pull-up.
    for (int i = 0; i < KEYBOARD_COLUMNS; i++) {
        GPIOoutConfigure(GPIOC, keyboard_column_pins[i], GPIO_OType_PP, GPIO_Low_Speed,
                         GPIO_PuPd_NOPULL);
    }
    keyboard_set_columns_low();

    // Setup keyboard rows: Input with pull-up, Falling edge exti.
    for (int i = 0; i < KEYBOARD_ROWS; i++) {
        GPIOinConfigure(GPIOC, keyboard_row_pins[i], GPIO_PuPd_UP, EXTI_Mode_Interrupt,
                        EXTI_Trigger_Falling);
    }

    // Turn on the row interrupts.
    NVIC_EnableIRQ(EXTI9_5_IRQn);
    NVIC_EnableIRQ(TIM3_IRQn);
}

int main(void) {
    LCDconfigure();
    setupKeyboardInterrupts();

    for (;;) {
        __WFI();
    }
}