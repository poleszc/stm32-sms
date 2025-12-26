#include "cyclic_buffer.h"
#include <gpio.h>
#include <stdbool.h>
#include <stm32.h>

// ===== LEDS CONFIG =====
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
enum LedRequest {
  NoRequest,
  RedReqLedOn,
  RedReqLedOff,
  RedReqLedChange,
  BlueReqLedOn,
  BlueReqLedOff,
  BlueReqLedChange,
  GreenBigReqLedOn,
  GreenBigReqLedOff,
  GreenBigReqLedChange,
  GreenSmallReqLedOn,
  GreenSmallReqLedOff,
  GreenSmallReqLedChange,
} typedef LedRequest;

#define BUFFER_SIZE 128
static char internal_in_buffer[BUFFER_SIZE];
static char internal_out_buffer[BUFFER_SIZE];
static CyclicBuffer_t in_buffer_struct;
static CyclicBuffer_t out_buffer_struct;

static CyclicBuffer_t *in_buffer = &in_buffer_struct;
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

  GPIOoutConfigure(RED_LED_GPIO, RED_LED_PIN, GPIO_OType_PP, GPIO_Low_Speed,
                   GPIO_PuPd_NOPULL);
  GPIOoutConfigure(BLUE_LED_GPIO, BLUE_LED_PIN, GPIO_OType_PP, GPIO_Low_Speed,
                   GPIO_PuPd_NOPULL);
  GPIOoutConfigure(GREEN_LED_GPIO, GREEN_LED_PIN, GPIO_OType_PP, GPIO_Low_Speed,
                   GPIO_PuPd_NOPULL);
  GPIOoutConfigure(GREEN2_LED_GPIO, GREEN2_LED_PIN, GPIO_OType_PP,
                   GPIO_Low_Speed, GPIO_PuPd_NOPULL);
}

static void setupUsart(void) {
  RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
  RCC->APB1ENR |= RCC_APB1ENR_USART2EN;

  GPIOafConfigure(GPIOA, 2, GPIO_OType_PP, GPIO_Fast_Speed, GPIO_PuPd_NOPULL,
                  GPIO_AF_USART2);
  GPIOafConfigure(GPIOA, 3, GPIO_OType_PP, GPIO_Fast_Speed, GPIO_PuPd_UP,
                  GPIO_AF_USART2);

  USART2->CR1 = USART_Mode_Rx_Tx | USART_WordLength_8b | USART_Parity_No;
  USART2->CR2 = USART_StopBits_1;
  USART2->CR3 = USART_FlowControl_None;
  USART2->BRR = (PCLK1_HZ + (BAUD / 2U)) / BAUD;
  USART2->CR1 |= USART_Enable;
}

static void setupButtons(void) {
  RCC->AHB1ENR |=
      RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN;
}

static void setupBuffers(void) {
  cycBufInit(in_buffer, internal_in_buffer, BUFFER_SIZE);
  cycBufInit(out_buffer, internal_out_buffer, BUFFER_SIZE);
}

// ==== UART =====
/**
 * @brief Read one char from UART and save it in the `in_buffer`.
 */
static void uartRead(void) {
  if (!cycBufFull(in_buffer) && (USART2->SR & USART_SR_RXNE)) {
    char c = USART2->DR;
    cycBufWrite(in_buffer, c);
  }
}


int main(void) {
  setupLeds();
  setupUsart();

  for (;;) {
  }
}