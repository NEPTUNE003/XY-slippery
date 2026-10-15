/**
 * @file plotter.h
 * @brief 二维滑台运动接口（点动 / 直线）
 *
 * 坐标系：原点在可画范围左下角，+X 向右，+Y 向上。
 */
#ifndef PLOTTER_H
#define PLOTTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void Plotter_Init(void);

/* 把当前机械位置记为 (0,0) */
void Plotter_SetOrigin(void);
uint8_t Plotter_IsOriginSet(void);

float Plotter_GetX(void);
float Plotter_GetY(void);

/* 点动：axis 0=X, 1=Y；dir_positive 1=正方向 */
void Plotter_JogStart(uint8_t axis, uint8_t dir_positive);
void Plotter_JogStop(uint8_t axis, uint8_t dir_positive);

/* TIM9 周期回调里调用（点动计步） */
void Plotter_OnTimPeriodElapsed(void);

/* 绝对坐标直线（两轴插补，斜线时同时发脉冲） */
void Plotter_LineTo(float x_mm, float y_mm);

/* 快速移动（当前与 LineTo 相同，预留） */
void Plotter_MoveTo(float x_mm, float y_mm);

/* 标定用：只发固定个数脉冲，不改软件坐标。axis 0=X 1=Y；dir_positive 1=正 */
void Plotter_PulseAxis(uint8_t axis, uint8_t dir_positive, uint32_t pulses);

#ifdef __cplusplus
}
#endif

#endif /* PLOTTER_H */
