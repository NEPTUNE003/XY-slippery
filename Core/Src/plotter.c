/**
 * @file plotter.c
 * @brief 运动引擎：点动用 TIM9 PWM；画线用同一时基 + Bresenham 同拍发脉冲
 *
 * 图案请写在 drawings.c。
 */
#include "plotter.h"
#include "main.h"
#include "tim.h"

/* 机械行程软限位：X≤20cm，Y≤16cm */
#define PLOT_X_MAX_MM           200.0f
#define PLOT_Y_MAX_MM           160.0f

/*
 * 步距（步/毫米）。标定：1000 脉冲实测走 20mm
 *   步距 = 1000 / 20 = 50
 */
#define PLOT_STEPS_PER_MM_X     50.0f
#define PLOT_STEPS_PER_MM_Y     50.0f
/*
 * 坐标系：原点=最左下角
 */
#define PLOT_X_DIR_POS          GPIO_PIN_RESET     // 正向（+X，向右）
#define PLOT_X_DIR_NEG          GPIO_PIN_SET       // 反向（−X，向左）

#define PLOT_Y_DIR_POS          GPIO_PIN_RESET     // +Y（向上）
#define PLOT_Y_DIR_NEG          GPIO_PIN_SET       // −Y（向下）

#define PLOT_PWM_ARR            999

/* PE5=Y(PUL1/TIM9_CH1), PE6=X(PUL2/TIM9_CH2) */
#define PUL_Y_Pin               GPIO_PIN_5
#define PUL_X_Pin               GPIO_PIN_6
#define PUL_GPIO_Port           GPIOE

extern TIM_HandleTypeDef htim9;

static float s_x_mm;
static float s_y_mm;
static uint8_t s_origin_ok;

/* 0=空闲 1=点动计步 2=单轴定步 3=直线 Bresenham */
static volatile uint8_t s_mode;
static volatile uint32_t s_steps_left;
static volatile int32_t s_br_ax;
static volatile int32_t s_br_ay;
static volatile int32_t s_br_err;
static volatile uint8_t s_br_x_major;
static volatile uint8_t s_do_x;
static volatile uint8_t s_do_y;

volatile uint32_t g_plot_steps_left;
volatile uint32_t g_plot_steps_done;
volatile uint8_t g_plot_busy;
volatile uint8_t g_plot_hold;

static void clamp_xy(float *x, float *y)
{
  if (s_origin_ok == 0)
  {
    return;
  }
  if (*x < 0.0f) { *x = 0.0f; }
  if (*y < 0.0f) { *y = 0.0f; }
  if (*x > PLOT_X_MAX_MM) { *x = PLOT_X_MAX_MM; }
  if (*y > PLOT_Y_MAX_MM) { *y = PLOT_Y_MAX_MM; }
}

static void pwm_stop_all(void)
{
  HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_1);
  HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_2);
  HAL_TIM_Base_Stop_IT(&htim9);
  s_mode = 0;
  s_steps_left = 0;
  g_plot_steps_left = 0;
  g_plot_busy = 0;
  g_plot_hold = 0;
}

static void pul_as_gpio(void)
{
  GPIO_InitTypeDef gpio = {0};

  HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_1);
  HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_2);

  gpio.Pin = PUL_X_Pin | PUL_Y_Pin;
  gpio.Mode = GPIO_MODE_OUTPUT_PP;
  gpio.Pull = GPIO_NOPULL;
  gpio.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(PUL_GPIO_Port, &gpio);
  HAL_GPIO_WritePin(PUL_GPIO_Port, PUL_X_Pin | PUL_Y_Pin, GPIO_PIN_RESET);
}

static void pul_as_tim(void)
{
  HAL_TIM_MspPostInit(&htim9);
  __HAL_TIM_SET_COMPARE(&htim9, TIM_CHANNEL_1, PLOT_PWM_ARR / 2);
  __HAL_TIM_SET_COMPARE(&htim9, TIM_CHANNEL_2, PLOT_PWM_ARR / 2);
}

static void pulse_edges(uint8_t do_x, uint8_t do_y)
{
  if (do_x != 0)
  {
    HAL_GPIO_WritePin(PUL_GPIO_Port, PUL_X_Pin, GPIO_PIN_SET);
  }
  if (do_y != 0)
  {
    HAL_GPIO_WritePin(PUL_GPIO_Port, PUL_Y_Pin, GPIO_PIN_SET);
  }
  /* TB6600 脉宽：约数微秒 */
  for (volatile uint32_t i = 0; i < 80; i++)
  {
  }
  if (do_x != 0)
  {
    HAL_GPIO_WritePin(PUL_GPIO_Port, PUL_X_Pin, GPIO_PIN_RESET);
  }
  if (do_y != 0)
  {
    HAL_GPIO_WritePin(PUL_GPIO_Port, PUL_Y_Pin, GPIO_PIN_RESET);
  }
}

/** 单轴连续走 steps 步（阻塞）——点动同款 PWM */
static void move_axis_steps(uint8_t axis_y, GPIO_PinState dir, uint32_t steps)
{
  if (steps == 0)
  {
    return;
  }

  pul_as_tim();
  pwm_stop_all();
  g_plot_hold = 0;
  s_mode = 2;
  s_steps_left = steps;
  g_plot_steps_left = steps;
  g_plot_busy = 1;

  if (axis_y != 0)
  {
    HAL_GPIO_WritePin(DIR1_GPIO_Port, DIR1_Pin, dir);
    HAL_TIM_Base_Start_IT(&htim9);
    HAL_TIM_PWM_Start(&htim9, TIM_CHANNEL_1);
  }
  else
  {
    HAL_GPIO_WritePin(DIR2_GPIO_Port, DIR2_Pin, dir);
    HAL_TIM_Base_Start_IT(&htim9);
    HAL_TIM_PWM_Start(&htim9, TIM_CHANNEL_2);
  }

  while (g_plot_busy != 0)
  {
  }
  pwm_stop_all();
}

/**
 * 整段直线一次跑完：Bresenham，每个时基节拍按需 X/Y 同时发沿。
 * 禁止“先跑完所有 X 再跑所有 Y”。
 */
static void move_line_steps(int32_t sx, int32_t sy)
{
  uint32_t ax;
  uint32_t ay;
  GPIO_PinState xdir;
  GPIO_PinState ydir;

  if ((sx == 0) && (sy == 0))
  {
    return;
  }

  xdir = (sx >= 0) ? PLOT_X_DIR_POS : PLOT_X_DIR_NEG;
  ydir = (sy >= 0) ? PLOT_Y_DIR_POS : PLOT_Y_DIR_NEG;
  ax = (sx >= 0) ? (uint32_t)sx : (uint32_t)(-sx);
  ay = (sy >= 0) ? (uint32_t)sy : (uint32_t)(-sy);

  if (ay == 0)
  {
    move_axis_steps(0, xdir, ax);
    return;
  }
  if (ax == 0)
  {
    move_axis_steps(1, ydir, ay);
    return;
  }

  pwm_stop_all();
  pul_as_gpio();

  HAL_GPIO_WritePin(DIR2_GPIO_Port, DIR2_Pin, xdir);
  HAL_GPIO_WritePin(DIR1_GPIO_Port, DIR1_Pin, ydir);

  s_br_ax = (int32_t)ax;
  s_br_ay = (int32_t)ay;
  if (ax >= ay)
  {
    s_br_x_major = 1;
    s_br_err = (int32_t)(ax / 2);
    s_steps_left = ax;
  }
  else
  {
    s_br_x_major = 0;
    s_br_err = (int32_t)(ay / 2);
    s_steps_left = ay;
  }

  g_plot_hold = 0;
  s_mode = 3;
  g_plot_steps_left = s_steps_left;
  g_plot_busy = 1;

  __HAL_TIM_SET_AUTORELOAD(&htim9, PLOT_PWM_ARR);
  HAL_TIM_GenerateEvent(&htim9, TIM_EVENTSOURCE_UPDATE);
  HAL_TIM_Base_Start_IT(&htim9);

  while (g_plot_busy != 0)
  {
  }

  pwm_stop_all();
  pul_as_tim();
}

static void line_to_raw(float x_mm, float y_mm)
{
  float dx_mm;
  float dy_mm;
  int32_t sx;
  int32_t sy;

  clamp_xy(&x_mm, &y_mm);
  dx_mm = x_mm - s_x_mm;
  dy_mm = y_mm - s_y_mm;

  sx = (int32_t)(dx_mm * PLOT_STEPS_PER_MM_X + ((dx_mm >= 0.0f) ? 0.5f : -0.5f));
  sy = (int32_t)(dy_mm * PLOT_STEPS_PER_MM_Y + ((dy_mm >= 0.0f) ? 0.5f : -0.5f));

  move_line_steps(sx, sy);

  s_x_mm = x_mm;
  s_y_mm = y_mm;
}

void Plotter_Init(void)
{
  s_x_mm = 0.0f;
  s_y_mm = 0.0f;
  s_origin_ok = 0;
  s_mode = 0;
  s_steps_left = 0;
  g_plot_steps_left = 0;
  g_plot_steps_done = 0;
  g_plot_busy = 0;
  g_plot_hold = 0;

  HAL_TIM_MspPostInit(&htim9);

  __HAL_TIM_SET_AUTORELOAD(&htim9, PLOT_PWM_ARR);
  __HAL_TIM_SET_COMPARE(&htim9, TIM_CHANNEL_1, PLOT_PWM_ARR / 2);
  __HAL_TIM_SET_COMPARE(&htim9, TIM_CHANNEL_2, PLOT_PWM_ARR / 2);
  HAL_TIM_GenerateEvent(&htim9, TIM_EVENTSOURCE_UPDATE);

  HAL_NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
}

void Plotter_SetOrigin(void)
{
  s_x_mm = 0.0f;
  s_y_mm = 0.0f;
  s_origin_ok = 1;
}

uint8_t Plotter_IsOriginSet(void)
{
  return s_origin_ok;
}

float Plotter_GetX(void)
{
  return s_x_mm;
}

float Plotter_GetY(void)
{
  return s_y_mm;
}

void Plotter_JogStart(uint8_t axis, uint8_t dir_positive)
{
  pul_as_tim();
  pwm_stop_all();
  g_plot_steps_done = 0;
  g_plot_hold = 1;
  s_mode = 1;
  g_plot_busy = 1;

  if (axis == 1)
  {
    HAL_GPIO_WritePin(DIR1_GPIO_Port, DIR1_Pin,
                      (dir_positive != 0) ? PLOT_Y_DIR_POS : PLOT_Y_DIR_NEG);
    HAL_TIM_Base_Start_IT(&htim9);
    HAL_TIM_PWM_Start(&htim9, TIM_CHANNEL_1);
  }
  else
  {
    HAL_GPIO_WritePin(DIR2_GPIO_Port, DIR2_Pin,
                      (dir_positive != 0) ? PLOT_X_DIR_POS : PLOT_X_DIR_NEG);
    HAL_TIM_Base_Start_IT(&htim9);
    HAL_TIM_PWM_Start(&htim9, TIM_CHANNEL_2);
  }
}

void Plotter_JogStop(uint8_t axis, uint8_t dir_positive)
{
  float moved_mm;
  float sign = (dir_positive != 0) ? 1.0f : -1.0f;

  HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_1);
  HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_2);
  HAL_TIM_Base_Stop_IT(&htim9);

  if (axis == 1)
  {
    moved_mm = ((float)g_plot_steps_done) / PLOT_STEPS_PER_MM_Y;
    s_y_mm += sign * moved_mm;
  }
  else
  {
    moved_mm = ((float)g_plot_steps_done) / PLOT_STEPS_PER_MM_X;
    s_x_mm += sign * moved_mm;
  }
  clamp_xy(&s_x_mm, &s_y_mm);

  s_mode = 0;
  g_plot_hold = 0;
  g_plot_busy = 0;
  g_plot_steps_left = 0;
  g_plot_steps_done = 0;
}

void Plotter_LineTo(float x_mm, float y_mm)
{
  line_to_raw(x_mm, y_mm);
}

void Plotter_MoveTo(float x_mm, float y_mm)
{
  line_to_raw(x_mm, y_mm);
}

void Plotter_PulseAxis(uint8_t axis, uint8_t dir_positive, uint32_t pulses)
{
  GPIO_PinState dir;

  if (axis == 1)
  {
    dir = (dir_positive != 0) ? PLOT_Y_DIR_POS : PLOT_Y_DIR_NEG;
  }
  else
  {
    dir = (dir_positive != 0) ? PLOT_X_DIR_POS : PLOT_X_DIR_NEG;
  }
  move_axis_steps(axis, dir, pulses);
}

void Plotter_OnTimPeriodElapsed(void)
{
  if (s_mode == 1)
  {
    g_plot_steps_done++;
    return;
  }

  if (s_mode == 2)
  {
    if (s_steps_left > 0)
    {
      s_steps_left--;
      g_plot_steps_left = s_steps_left;
      if (s_steps_left == 0)
      {
        HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_1);
        HAL_TIM_PWM_Stop(&htim9, TIM_CHANNEL_2);
        HAL_TIM_Base_Stop_IT(&htim9);
        s_mode = 0;
        g_plot_busy = 0;
      }
    }
    return;
  }

  if (s_mode == 3)
  {
    s_do_x = 0;
    s_do_y = 0;

    if (s_br_x_major != 0)
    {
      s_do_x = 1;
      s_br_err -= s_br_ay;
      if (s_br_err < 0)
      {
        s_do_y = 1;
        s_br_err += s_br_ax;
      }
    }
    else
    {
      s_do_y = 1;
      s_br_err -= s_br_ax;
      if (s_br_err < 0)
      {
        s_do_x = 1;
        s_br_err += s_br_ay;
      }
    }

    pulse_edges(s_do_x, s_do_y);

    if (s_steps_left > 0)
    {
      s_steps_left--;
      g_plot_steps_left = s_steps_left;
    }
    if (s_steps_left == 0)
    {
      HAL_TIM_Base_Stop_IT(&htim9);
      s_mode = 0;
      g_plot_busy = 0;
    }
  }
}
