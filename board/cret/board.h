/* Copyright 2021 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Waddledoo board configuration */

#ifndef __CROS_EC_BOARD_H
#define __CROS_EC_BOARD_H

#define VARIANT_DEDEDE_EC_NPCX796FC
#include "baseboard.h"

/*
 * Keep the system unlocked in early development.
 * TODO(b/151264302): Make sure to remove this before production!
 */
#define CONFIG_SYSTEM_UNLOCKED

/* Save some flash space */
#define CONFIG_CHIP_INIT_ROM_REGION
#define CONFIG_DEBUG_ASSERT_BRIEF

/* EC console commands */
#define CONFIG_CMD_TCPC_DUMP
#define CONFIG_CMD_CHARGER_DUMP

/* Battery */
#define CONFIG_BATTERY_FUEL_GAUGE

/* Charger */
#define CONFIG_CHARGER_RAA489000
#define CONFIG_CHARGER_SENSE_RESISTOR_AC 10
#define CONFIG_CHARGER_SENSE_RESISTOR 10
#undef CONFIG_USB_PD_TCPC_LPM_EXIT_DEBOUNCE
#define CONFIG_USB_PD_TCPC_LPM_EXIT_DEBOUNCE (100 * MSEC)

/*
 * GPIO for C1 interrupts, for baseboard use
 *
 * Note this line might already have its pull up disabled for HDMI DBs, but
 * it should be fine to set again before z-state.
 */
#define GPIO_EC_HDMI_EN_ODL GPIO_EC_I2C_SBU_USB_C1_SCL
#define GPIO_HDMI_PP3300_EN GPIO_SUB_USB_C1_INT_ODL

/* PWM */
#define CONFIG_PWM
#define NPCX7_PWM1_SEL    0  /* GPIO C2 is used as PWM1. */

/* USB */
#define CONFIG_BC12_DETECT_PI3USB9201

/* LED */
#define CONFIG_LED_COMMON
#define CONFIG_LED_ONOFF_STATES_BAT_LOW 10
#define CONFIG_LED_ONOFF_STATES

/* USB PD */
#define CONFIG_USB_PD_PORT_MAX_COUNT 1
#define CONFIG_USB_PD_TCPM_RAA489000

/* USB defines specific to external TCPCs */
#define CONFIG_USB_PD_DUAL_ROLE_AUTO_TOGGLE
#define CONFIG_USB_PD_VBUS_DETECT_TCPC
#define CONFIG_USB_PD_DISCHARGE_TCPC
#define CONFIG_USB_PD_TCPC_LOW_POWER

/* Variant references the TCPCs to determine Vbus sourcing */
#define CONFIG_USB_PD_5V_EN_CUSTOM

#undef PD_POWER_SUPPLY_TURN_ON_DELAY
#undef PD_POWER_SUPPLY_TURN_OFF_DELAY
#undef CONFIG_USBC_VCONN_SWAP_DELAY_US
/* 20% margin added for these timings */
#define PD_POWER_SUPPLY_TURN_ON_DELAY	13080	/* us */
#define PD_POWER_SUPPLY_TURN_OFF_DELAY	16080	/* us */
#undef CONFIG_USBC_VCONN_SWAP_DELAY_US
#define CONFIG_USBC_VCONN_SWAP_DELAY_US		787	/* us */


/* I2C configuration */
#define I2C_PORT_EEPROM     NPCX_I2C_PORT7_0
#define I2C_PORT_BATTERY    NPCX_I2C_PORT5_0
#define I2C_PORT_SENSOR     NPCX_I2C_PORT0_0
#define I2C_PORT_USB_C0     NPCX_I2C_PORT1_0
#define I2C_PORT_SUB_USB_C1 NPCX_I2C_PORT2_0
#define I2C_PORT_USB_MUX    I2C_PORT_USB_C0
/* TODO(b:147440290): Need to handle multiple charger ICs */
#define I2C_PORT_CHARGER    I2C_PORT_USB_C0

#define I2C_PORT_ACCEL      I2C_PORT_SENSOR

#define I2C_ADDR_EEPROM_FLAGS 0x50 /* 7b address */

/* Sensors */
#define CONFIG_CMD_ACCELS
#define CONFIG_CMD_ACCEL_INFO

#define CONFIG_ACCEL_LIS2DE		/* Lid accel */
#define CONFIG_ACCELGYRO_LSM6DSO	/* Base accel */
#define CONFIG_ACCEL_LSM6DSO_INT_EVENT \
	TASK_EVENT_MOTION_SENSOR_INTERRUPT(BASE_ACCEL)

/* Lid operates in forced mode, base in FIFO */
#define CONFIG_ACCEL_FORCE_MODE_MASK BIT(LID_ACCEL)
#define CONFIG_ACCEL_FIFO
#define CONFIG_ACCEL_FIFO_SIZE 256	/* Must be a power of 2 */
#define CONFIG_ACCEL_FIFO_THRES (CONFIG_ACCEL_FIFO_SIZE / 3)

#define CONFIG_ACCEL_INTERRUPTS
#define CONFIG_ACCELGYRO_BMI160_INT_EVENT \
	TASK_EVENT_MOTION_SENSOR_INTERRUPT(BASE_ACCEL)

#define CONFIG_DYNAMIC_MOTION_SENSOR_COUNT

#define CONFIG_LID_ANGLE
#define CONFIG_LID_ANGLE_UPDATE
#define CONFIG_LID_ANGLE_SENSOR_BASE BASE_ACCEL
#define CONFIG_LID_ANGLE_SENSOR_LID LID_ACCEL

#define CONFIG_TABLET_MODE
#define CONFIG_TABLET_MODE_SWITCH
#define CONFIG_GMR_TABLET_MODE

/* Thermistors */
#define CONFIG_TEMP_SENSOR
#define CONFIG_THERMISTOR
#define CONFIG_STEINHART_HART_3V3_51K1_47K_4050B
#define CONFIG_TEMP_SENSOR_POWER_GPIO GPIO_EN_PP3300_A

#ifndef __ASSEMBLER__

#include "gpio_signal.h"
#include "registers.h"

enum adc_channel {
	ADC_TEMP_SENSOR_1,     /* ADC0 */
	ADC_TEMP_SENSOR_2,     /* ADC1 */
	ADC_VSNS_PP3300_A,     /* ADC9 */
	ADC_CH_COUNT
};

enum sensor_id {
	LID_ACCEL,
	BASE_ACCEL,
	BASE_GYRO,
	SENSOR_COUNT
};

enum temp_sensor_id {
	TEMP_SENSOR_1,
	TEMP_SENSOR_2,
	TEMP_SENSOR_COUNT
};

enum pwm_channel {
	PWM_CH_KBLIGHT,
	PWM_CH_COUNT,
};

/* List of possible batteries */
enum battery_type {
	BATTERY_BYD_1VX1H,
	BATTERY_BYD_X0Y5M,
	BATTERY_LGC,
	BATTERY_SWD_ATL,
	BATTERY_SWD_COS,
	BATTERY_SMP_ATL_VM732,
	BATTERY_SMP_ATL_RF9H3,
	BATTERY_SMP_COS_VM732,
	BATTERY_SMP_COS_RF9H3,
	BATTERY_BYD16,
	BATTERY_LGC3,
	BATTERY_SIMPLO,
	BATTERY_SIMPLO_LS,
	BATTERY_TYPE_COUNT,
};

#endif /* !__ASSEMBLER__ */
#endif /* __CROS_EC_BOARD_H */
