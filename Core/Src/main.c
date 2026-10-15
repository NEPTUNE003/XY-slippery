/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body —— 按键菜单；图案请改 drawings.c
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "tim.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "plotter.h"
#include "drawings.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef void (*DrawingFn)(void);
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define KEY_MODE_PORT       GPIOC
#define KEY_MODE_PIN        GPIO_PIN_13
#define KEY_OK_PORT         GPIOA
#define KEY_OK_PIN          GPIO_PIN_0
#define KEY_DEBOUNCE_MS     20

/* 标定模式每次发出的脉冲数（整数好算）。量完毫米后：步距 = 1000 / 实测毫米 */
#define CALIB_PULSES        1000

enum
{
  MODE_JOG_XP = 0,
  MODE_JOG_XN,
  MODE_JOG_YP,
  MODE_JOG_YN,
  MODE_SET_ORIGIN,
  MODE_DRAW_LEAF,
  MODE_CALIB_X,   /* 固定发 CALIB_PULSES 个脉冲，X+ */
  MODE_CALIB_Y,   /* 固定发 CALIB_PULSES 个脉冲，Y+ */
  MODE_COUNT
};

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
static uint8_t g_mode = MODE_JOG_XP;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void Key_Init(void);
static void Buzzer_Beep(uint8_t times);
static uint8_t Key_ReadMode(void);
static uint8_t Key_ReadOk(void);
static uint8_t Key_WaitClick(uint8_t (*read_fn)(void));
static void App_HoldJog(uint8_t axis, uint8_t dir_positive);
static void App_RunDrawing(DrawingFn fn);
static void App_OnOk(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static void Buzzer_Beep(uint8_t times)
{
  GPIO_InitTypeDef gpio = {0};
  uint8_t i;

  __HAL_RCC_GPIOG_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOG, &gpio);

  for (i = 0; i < times; i++)
  {
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_7, GPIO_PIN_SET);
    HAL_Delay(70);
    HAL_GPIO_WritePin(GPIOG, GPIO_PIN_7, GPIO_PIN_RESET);
    HAL_Delay(100);
  }
}

static void Key_Init(void)
{
  GPIO_InitTypeDef gpio = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  gpio.Mode = GPIO_MODE_INPUT;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_LOW;
  gpio.Pin = KEY_MODE_PIN;
  HAL_GPIO_Init(KEY_MODE_PORT, &gpio);
  gpio.Pin = KEY_OK_PIN;
  HAL_GPIO_Init(KEY_OK_PORT, &gpio);

  Buzzer_Beep(1);
}

static uint8_t Key_ReadMode(void)
{
  return (HAL_GPIO_ReadPin(KEY_MODE_PORT, KEY_MODE_PIN) == GPIO_PIN_SET) ? 1 : 0;
}

static uint8_t Key_ReadOk(void)
{
  return (HAL_GPIO_ReadPin(KEY_OK_PORT, KEY_OK_PIN) == GPIO_PIN_SET) ? 1 : 0;
}

static uint8_t Key_WaitClick(uint8_t (*read_fn)(void))
{
  if (read_fn() == 0)
  {
    return 0;
  }
  HAL_Delay(KEY_DEBOUNCE_MS);
  if (read_fn() == 0)
  {
    return 0;
  }
  while (read_fn() != 0)
  {
  }
  HAL_Delay(KEY_DEBOUNCE_MS);
  return 1;
}

static void App_HoldJog(uint8_t axis, uint8_t dir_positive)
{
  if (Key_ReadOk() == 0)
  {
    return;
  }
  HAL_Delay(KEY_DEBOUNCE_MS);
  if (Key_ReadOk() == 0)
  {
    return;
  }

  Plotter_JogStart(axis, dir_positive);
  while (Key_ReadOk() != 0)
  {
  }
  Plotter_JogStop(axis, dir_positive);
  HAL_Delay(KEY_DEBOUNCE_MS);
}

/** 统一画图入口：检查原点 → 提示音 → 调用图案函数 */
static void App_RunDrawing(DrawingFn fn)
{
  if (Plotter_IsOriginSet() == 0)
  {
    Buzzer_Beep(3);
    return;
  }
  Buzzer_Beep(2);
  fn();
  Buzzer_Beep(1);
}

static void App_OnOk(void)
{
  switch (g_mode)
  {
    case MODE_JOG_XP: App_HoldJog(0, 1); break;
    case MODE_JOG_XN: App_HoldJog(0, 0); break;
    case MODE_JOG_YP: App_HoldJog(1, 1); break;
    case MODE_JOG_YN: App_HoldJog(1, 0); break;
    case MODE_SET_ORIGIN:
      Plotter_SetOrigin();
      Buzzer_Beep(2);
      break;
    case MODE_DRAW_LEAF:
      App_RunDrawing(Drawing_Leaf);
      break;
    case MODE_CALIB_X:
      /* 发出恰好 1000 个脉冲，用尺子量走了多少 mm → 步距=1000/毫米 */
      Buzzer_Beep(2);
      Plotter_PulseAxis(0, 1, CALIB_PULSES);
      Buzzer_Beep(1);
      break;
    case MODE_CALIB_Y:
      Buzzer_Beep(2);
      Plotter_PulseAxis(1, 1, CALIB_PULSES);
      Buzzer_Beep(1);
      break;
    default:
      break;
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM9)
  {
    Plotter_OnTimPeriodElapsed();
  }
}

/* USER CODE END 0 */

int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  HAL_Init();

  /* USER CODE BEGIN Init */
  /* USER CODE END Init */

  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* USER CODE END SysInit */

  MX_GPIO_Init();
  MX_TIM9_Init();
  /* USER CODE BEGIN 2 */

  Plotter_Init();
  Key_Init();
  g_mode = MODE_JOG_XP;

  /* USER CODE END 2 */

  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (Key_WaitClick(Key_ReadMode) != 0)
    {
      g_mode++;
      if (g_mode >= MODE_COUNT)
      {
        g_mode = MODE_JOG_XP;
      }
      Buzzer_Beep((uint8_t)(g_mode + 1));
    }

    if (g_mode <= MODE_JOG_YN)
    {
      if (Key_ReadOk() != 0)
      {
        App_OnOk();
      }
    }
    else if (Key_WaitClick(Key_ReadOk) != 0)
    {
      App_OnOk();
    }
  }
  /* USER CODE END 3 */
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 25;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 7;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */
/* USER CODE END 4 */

void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  (void)file;
  (void)line;
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
