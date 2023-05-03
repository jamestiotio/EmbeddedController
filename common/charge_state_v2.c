/* Copyright 2014 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Battery charging task and state machine.
 */

#include "battery.h"
#include "battery_smart.h"
#include "builtin/assert.h"
#include "charge_manager.h"
#include "charge_state.h"
#include "charger.h"
#include "charger_profile_override.h"
#include "chipset.h"
#include "common.h"
#include "console.h"
#include "ec_commands.h"
#include "ec_ec_comm_client.h"
#include "ec_ec_comm_server.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "math_util.h"
#include "power.h"
#include "printf.h"
#include "system.h"
#include "task.h"
#include "throttle_ap.h"
#include "timer.h"
#include "usb_common.h"
#include "usb_pd.h"
#include "util.h"

/* Console output macros */
#define CPUTS(outstr) cputs(CC_CHARGER, outstr)
#define CPRINTS(format, args...) cprints(CC_CHARGER, format, ##args)
#define CPRINTF(format, args...) cprintf(CC_CHARGER, format, ##args)

/* Extra debugging prints when allocating power between lid and base. */
#undef CHARGE_ALLOCATE_EXTRA_DEBUG

#define CRITICAL_BATTERY_SHUTDOWN_TIMEOUT_US \
	(CONFIG_BATTERY_CRITICAL_SHUTDOWN_TIMEOUT * SECOND)
#define PRECHARGE_TIMEOUT_US (PRECHARGE_TIMEOUT * SECOND)

#ifdef CONFIG_THROTTLE_AP_ON_BAT_DISCHG_CURRENT
#ifndef CONFIG_HOSTCMD_EVENTS
#error "CONFIG_THROTTLE_AP_ON_BAT_DISCHG_CURRENT needs CONFIG_HOSTCMD_EVENTS"
#endif /* CONFIG_HOSTCMD_EVENTS */
#define BAT_OCP_TIMEOUT_US (60 * SECOND)
/* BAT_OCP_HYSTERESIS_PCT can be optionally overridden in board.h. */
#ifndef BAT_OCP_HYSTERESIS_PCT
#define BAT_OCP_HYSTERESIS_PCT 10
#endif /* BAT_OCP_HYSTERESIS_PCT */
#define BAT_OCP_HYSTERESIS \
	(BAT_MAX_DISCHG_CURRENT * BAT_OCP_HYSTERESIS_PCT / 100) /* mA */
#endif /* CONFIG_THROTTLE_AP_ON_BAT_DISCHG_CURRENT */

#ifdef CONFIG_THROTTLE_AP_ON_BAT_VOLTAGE
#ifndef CONFIG_HOSTCMD_EVENTS
#error "CONFIG_THROTTLE_AP_ON_BAT_VOLTAGE needs CONFIG_HOSTCMD_EVENTS"
#endif /* CONFIG_HOSTCMD_EVENTS */
#define BAT_UVP_TIMEOUT_US (60 * SECOND)
/* BAT_UVP_HYSTERESIS_PCT can be optionally overridden in board.h. */
#ifndef BAT_UVP_HYSTERESIS_PCT
#define BAT_UVP_HYSTERESIS_PCT 3
#endif /* BAT_UVP_HYSTERESIS_PCT */
#define BAT_UVP_HYSTERESIS \
	(BAT_LOW_VOLTAGE_THRESH * BAT_UVP_HYSTERESIS_PCT / 100) /* mV */
static timestamp_t uvp_throttle_start_time;
#endif /* CONFIG_THROTTLE_AP_ON_BAT_OLTAGE */

static int charge_request(int voltage, int current);

static uint8_t battery_level_shutdown;

/*
 * State for charger_task(). Here so we can reset it on a HOOK_INIT, and
 * because stack space is more limited than .bss
 */
static const struct battery_info *batt_info;
static struct charge_state_data curr;
static enum charge_state_v2 prev_state;
static int prev_ac, prev_charge, prev_full, prev_disp_charge;
static enum battery_present prev_bp;
static int is_full; /* battery not accepting current */
static enum ec_charge_control_mode chg_ctl_mode;
static int manual_voltage; /* Manual voltage override (-1 = no override) */
static int manual_current; /* Manual current override (-1 = no override) */
static unsigned int user_current_limit = -1U;
test_export_static timestamp_t shutdown_target_time;
static bool is_charging_progress_displayed;
static timestamp_t precharge_start_time;
static struct sustain_soc sustain_soc;
static struct current_limit {
	uint32_t value; /* Charge limit to apply, in mA */
	int soc; /* Minimum battery SoC at which the limit will be applied. */
} current_limit = { -1U, 0 };

/*
 * The timestamp when the battery charging current becomes stable.
 * When a new charging status happens, charger needs several seconds to
 * stabilize the battery charging current.
 * stable_current should be evaluated when stable_ts expired.
 * stable_ts should be reset if the charger input voltage/current changes,
 * or a new battery charging voltage/request happened.
 * By evaluating stable_current, we can evaluate the battery's desired charging
 * power desired_mw. This allow us to have a better charging efficiency by
 * negotiating the most fit PDO, i.e. the PDO provides the power just enough for
 * the system and battery, or the PDO with preferred voltage.
 */
STATIC_IF(CONFIG_USB_PD_PREFER_MV) timestamp_t stable_ts;
/* battery charging current evaluated after stable_ts expired */
STATIC_IF(CONFIG_USB_PD_PREFER_MV) int stable_current;
/* battery desired power in mW. This is used to negotiate the suitable PDO */
STATIC_IF(CONFIG_USB_PD_PREFER_MV) int desired_mw;
STATIC_IF_NOT(CONFIG_USB_PD_PREFER_MV) struct pd_pref_config_t pd_pref_config;

#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
static int base_connected;
/* Base has responded to one of our commands already. */
static int base_responsive;
static int charge_base;
static int prev_charge_base;
static int prev_current_base;
static int prev_allow_charge_base;
static int prev_current_lid;

/*
 * In debugging mode, with AC, input current to allocate to base. Negative
 * value disables manual mode.
 */
static int manual_ac_current_base = -1;
/*
 * In debugging mode, when discharging, current to transfer from lid to base
 * (negative to transfer from base to lid). Only valid when enabled is true.
 */
static int manual_noac_enabled;
static int manual_noac_current_base;
#else
static const int base_connected;
#endif

/* Is battery connected but unresponsive after precharge? */
static int battery_seems_dead;

static int battery_seems_disconnected;

/*
 * Was battery removed?  Set when we see BP_NO, cleared after the battery is
 * reattached and becomes responsive.  Used to indicate an error state after
 * removal and trigger re-reading the battery static info when battery is
 * reattached and responsive.
 */
static int battery_was_removed;

static int problems_exist;
static int debugging;

static const char *const prob_text[] = {
	"static update",     "set voltage",	 "set current", "set mode",
	"set input current", "post init",	 "chg params",	"batt params",
	"custom profile",    "cfg secondary chg"
};
BUILD_ASSERT(ARRAY_SIZE(prob_text) == NUM_PROBLEM_TYPES);

/*
 * TODO(crosbug.com/p/27639): When do we decide a problem is real and not
 * just intermittent? And what do we do about it?
 */
void charge_problem(enum problem_type p, int v)
{
	static int last_prob_val[NUM_PROBLEM_TYPES];
	static timestamp_t last_prob_time[NUM_PROBLEM_TYPES];
	timestamp_t t_now, t_diff;

	if (last_prob_val[p] != v) {
		t_now = get_time();
		t_diff.val = t_now.val - last_prob_time[p].val;
		CPRINTS("charge problem: %s, 0x%x -> 0x%x after %.6" PRId64 "s",
			prob_text[p], last_prob_val[p], v, t_diff.val);
		last_prob_val[p] = v;
		last_prob_time[p] = t_now;
	}
	problems_exist = 1;
}

enum ec_charge_control_mode get_chg_ctrl_mode(void)
{
	return chg_ctl_mode;
}

void reset_prev_disp_charge(void)
{
	prev_disp_charge = -1;
}

static int battery_sustainer_set(int8_t lower, int8_t upper)
{
	if (lower == -1 || upper == -1) {
		CPRINTS("Sustainer disabled");
		sustain_soc.lower = -1;
		sustain_soc.upper = -1;
		return EC_SUCCESS;
	}

	if (lower <= upper && 0 <= lower && upper <= 100) {
		/* Currently sustainer requires discharge_on_ac. */
		if (!IS_ENABLED(CONFIG_CHARGER_DISCHARGE_ON_AC))
			return EC_RES_UNAVAILABLE;
		sustain_soc.lower = lower;
		sustain_soc.upper = upper;
		CPRINTS("Sustainer set: %d%% ~ %d%%", lower, upper);
		return EC_SUCCESS;
	}

	CPRINTS("Invalid param: %s(%d, %d)", __func__, lower, upper);
	return EC_ERROR_INVAL;
}

static void battery_sustainer_disable(void)
{
	battery_sustainer_set(-1, -1);
}

static bool battery_sustainer_enabled(void)
{
	return sustain_soc.lower != -1 && sustain_soc.upper != -1;
}

#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
/*
 * Parameters for dual-battery policy.
 * TODO(b:71881017): This should be made configurable by AP in the future.
 */
struct dual_battery_policy {
	/*** Policies when AC is not connected. ***/
	/* Voltage to use when using OTG mode between lid and base (mV) */
	uint16_t otg_voltage;
	/* Maximum current to apply from base to lid (mA) */
	uint16_t max_base_to_lid_current;
	/*
	 * Margin to apply between provided OTG output current and input current
	 * limit, to make sure that input charger does not overcurrent output
	 * charger. input_current = (1-margin) * output_current. (/128)
	 */
	uint8_t margin_otg_current;

	/* Only do base to lid OTG when base battery above this value (%) */
	uint8_t min_charge_base_otg;

	/*
	 * When base/lid battery percentage is below this value, do
	 * battery-to-battery charging. (%)
	 */
	uint8_t max_charge_base_batt_to_batt;
	uint8_t max_charge_lid_batt_to_batt;

	/*** Policies when AC is connected. ***/
	/* Minimum power to allocate to base (mW), includes some margin to allow
	 * base to charge when critically low.
	 */
	uint16_t min_base_system_power;

	/* Smoothing factor for lid power (/128) */
	uint8_t lid_system_power_smooth;
	/*
	 * Smoothing factor for base/lid battery power, when the battery power
	 * is decreasing only: we try to estimate the maximum power that the
	 * battery is willing to take and always reset it when it draws more
	 * than the estimate. (/128)
	 */
	uint8_t battery_power_smooth;

	/*
	 * Margin to add to requested base/lid battery power, to figure out how
	 * much current to allocate. allocation = (1+margin) * request. (/128)
	 */
	uint8_t margin_base_battery_power;
	uint8_t margin_lid_battery_power;

	/* Maximum current to apply from lid to base (mA) */
	uint16_t max_lid_to_base_current;
};

static const struct dual_battery_policy db_policy = {
	.otg_voltage = 12000, /* mV */
	.max_base_to_lid_current = 1800, /* mA, about 2000mA with margin. */
	.margin_otg_current = 13, /* /128 = 10.1% */
	.min_charge_base_otg = 5, /* % */
	.max_charge_base_batt_to_batt = 4, /* % */
	.max_charge_lid_batt_to_batt = 10, /* % */
	.min_base_system_power = 1300, /* mW */
	.lid_system_power_smooth = 32, /* 32/128 = 0.25 */
	.battery_power_smooth = 1, /* 1/128 = 0.008 */
	.margin_base_battery_power = 32, /* 32/128 = 0.25 */
	.margin_lid_battery_power = 32, /* 32/128 = 0.25 */
	.max_lid_to_base_current = 2000, /* mA */
};

/* Add at most "value" to power_var, subtracting from total_power budget. */
#define CHG_ALLOCATE(power_var, total_power, value)       \
	do {                                              \
		int val_capped = MIN(value, total_power); \
		(power_var) += val_capped;                \
		(total_power) -= val_capped;              \
	} while (0)

/**
 * Setup current settings for base, and record previous values, if the base
 * is responsive.
 *
 * @param current_base Current to be drawn by base (negative to provide power)
 * @param allow_charge_base Whether base battery should be charged (only makes
 *                          sense with positive current)
 */
static int set_base_current(int current_base, int allow_charge_base)
{
	/* "OTG" voltage from base to lid. */
	const int otg_voltage = db_policy.otg_voltage;
	int ret;

	ret = ec_ec_client_base_charge_control(current_base, otg_voltage,
					       allow_charge_base);
	if (ret) {
		/* Ignore errors until the base is responsive. */
		if (base_responsive)
			return ret;
	} else {
		base_responsive = 1;
		prev_current_base = current_base;
		prev_allow_charge_base = allow_charge_base;
	}

	return EC_RES_SUCCESS;
}

/**
 * Setup current settings for lid and base, in a safe way.
 *
 * @param current_base Current to be drawn by base (negative to provide power)
 * @param allow_charge_base Whether base battery should be charged (only makes
 *                          sense with positive current)
 * @param current_lid Current to be drawn by lid (negative to provide power)
 * @param allow_charge_lid Whether lid battery should be charged
 */
static void set_base_lid_current(int current_base, int allow_charge_base,
				 int current_lid, int allow_charge_lid)
{
	/* "OTG" voltage from lid to base. */
	const int otg_voltage = db_policy.otg_voltage;

	int lid_first;
	int ret;
	int chgnum = 0;

	/* TODO(b:71881017): This is still quite verbose during charging. */
	if (prev_current_base != current_base ||
	    prev_allow_charge_base != allow_charge_base ||
	    prev_current_lid != current_lid) {
		CPRINTS("Base/Lid: %d%s/%d%s mA", current_base,
			allow_charge_base ? "+" : "", current_lid,
			allow_charge_lid ? "+" : "");
	}

	/*
	 * To decide whether to first control the lid or the base, we first
	 * control the side that _reduces_ current that would be drawn, then
	 * setup one that would start providing power, then increase current.
	 */
	if (current_lid >= 0 && current_lid < prev_current_lid)
		lid_first = 1; /* Lid decreases current */
	else if (current_base >= 0 && current_base < prev_current_base)
		lid_first = 0; /* Base decreases current */
	else if (current_lid < 0)
		lid_first = 1; /* Lid provide power */
	else
		lid_first = 0; /* All other cases: control the base first */

	if (!lid_first && base_connected) {
		ret = set_base_current(current_base, allow_charge_base);
		if (ret)
			return;
	}

	if (current_lid >= 0) {
		ret = charge_set_output_current_limit(CHARGER_SOLO, 0, 0);
		if (ret)
			return;
		ret = charger_set_input_current_limit(chgnum, current_lid);
		if (ret)
			return;
		if (allow_charge_lid)
			ret = charge_request(curr.requested_voltage,
					     curr.requested_current);
		else
			ret = charge_request(0, 0);
	} else {
		ret = charge_set_output_current_limit(
			CHARGER_SOLO, -current_lid, otg_voltage);
	}

	if (ret)
		return;

	prev_current_lid = current_lid;

	if (lid_first && base_connected) {
		ret = set_base_current(current_base, allow_charge_base);
		if (ret)
			return;
	}

	/*
	 * Make sure cross-power is enabled (it might not be enabled right after
	 * plugging the base, or when an adapter just got connected).
	 */
	if (base_connected && current_base != 0)
		board_enable_base_power(1);
}

/**
 * Smooth power value, covering some edge cases.
 * Compute s*curr+(1-s)*prev, where s is in 1/128 unit.
 */
static int smooth_value(int prev, int curr, int s)
{
	if (curr < 0)
		curr = 0;
	if (prev < 0)
		return curr;

	return prev + s * (curr - prev) / 128;
}

/**
 * Add margin m to value. Compute (1+m)*value, where m is in 1/128 unit.
 */
static int add_margin(int value, int m)
{
	return value + m * value / 128;
}
#endif /* CONFIG_EC_EC_COMM_BATTERY_CLIENT */

/* allocate power between the base and the lid */
static void base_charge_allocate_input_current_limit(void)
{
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	/*
	 * All the power numbers are in mW.
	 *
	 * Since we work with current and voltage in mA and mV, multiplying them
	 * gives numbers in uW, which are dangerously close to overflowing when
	 * doing intermediate computations (60W * 100 overflows a 32-bit int,
	 * for example). We therefore divide the product by 1000 and re-multiply
	 * the power numbers by 1000 when converting them back to current.
	 */
	int total_power = 0;

	static int prev_base_battery_power = -1;
	int base_battery_power = 0;
	int base_battery_power_max = 0;

	static int prev_lid_system_power = -1;
	int lid_system_power;

	static int prev_lid_battery_power = -1;
	int lid_battery_power = 0;
	int lid_battery_power_max = 0;

	int power_base = 0;
	int power_lid = 0;

	int current_base = 0;
	int current_lid = 0;

	int charge_lid = charge_get_percent();

	const struct ec_response_battery_dynamic_info *const base_bd =
		&battery_dynamic[BATT_IDX_BASE];

	if (!base_connected) {
		set_base_lid_current(0, 0, curr.desired_input_current, 1);
		prev_base_battery_power = -1;
		return;
	}

	/* Charging */
	if (curr.desired_input_current > 0 && curr.input_voltage > 0)
		total_power =
			curr.desired_input_current * curr.input_voltage / 1000;

	/*
	 * TODO(b:71723024): We should be able to replace this test by curr.ac,
	 * but the value is currently wrong, especially during transitions.
	 */
	if (total_power <= 0) {
		int base_critical =
			charge_base >= 0 &&
			charge_base < db_policy.max_charge_base_batt_to_batt;

		/* Discharging */
		prev_base_battery_power = -1;
		prev_lid_system_power = -1;
		prev_lid_battery_power = -1;

		/* Manual control */
		if (manual_noac_enabled) {
			int lid_current, base_current;

			if (manual_noac_current_base > 0) {
				base_current = -manual_noac_current_base;
				lid_current = add_margin(
					manual_noac_current_base,
					db_policy.margin_otg_current);
			} else {
				lid_current = manual_noac_current_base;
				base_current = add_margin(
					-manual_noac_current_base,
					db_policy.margin_otg_current);
			}

			set_base_lid_current(base_current, 0, lid_current, 0);
			return;
		}

		/*
		 * System is off, cut power to the base. We'll reset the base
		 * when system restarts, or when AC is plugged.
		 */
		if (chipset_in_state(CHIPSET_STATE_ANY_OFF)) {
			set_base_lid_current(0, 0, 0, 0);
			if (base_responsive) {
				/* Base still responsive, put it to sleep. */
				CPRINTF("Hibernating base\n");
				ec_ec_client_hibernate();
				base_responsive = 0;
				board_enable_base_power(0);
			}
			return;
		}

		/*
		 * System is suspended, let the lid and base run on their
		 * own power. However, if the base battery is critically low, we
		 * still want to provide power to the base, to make sure it
		 * stays alive to be able to wake the system on keyboard or
		 * touchpad events.
		 */
		if (chipset_in_state(CHIPSET_STATE_ANY_SUSPEND) &&
		    !base_critical) {
			set_base_lid_current(0, 0, 0, 0);
			return;
		}

		if (charge_base > db_policy.min_charge_base_otg) {
			int lid_current = db_policy.max_base_to_lid_current;
			int base_current = add_margin(
				lid_current, db_policy.margin_otg_current);
			/* Draw current from base to lid */
			set_base_lid_current(
				-base_current, 0, lid_current,
				charge_lid <
					db_policy.max_charge_lid_batt_to_batt);
		} else {
			/*
			 * Base battery is too low, apply power to it, and allow
			 * it to charge if it is critically low.
			 *
			 * TODO(b:71881017): When suspended, this will make the
			 * battery charge oscillate between 3 and 4 percent,
			 * which might not be great for battery life. We need
			 * some hysteresis.
			 */
			/*
			 * TODO(b:71881017): Precompute (ideally, at build time)
			 * the base_current, so we do not need to do a division
			 * here.
			 */
			int base_current =
				(db_policy.min_base_system_power * 1000) /
				db_policy.otg_voltage;
			int lid_current = add_margin(
				base_current, db_policy.margin_otg_current);

			set_base_lid_current(base_current, base_critical,
					     -lid_current, 0);
		}

		return;
	}

	/* Manual control */
	if (manual_ac_current_base >= 0) {
		int current_base = manual_ac_current_base;
		int current_lid =
			curr.desired_input_current - manual_ac_current_base;

		if (current_lid < 0) {
			current_base = curr.desired_input_current;
			current_lid = 0;
		}

		set_base_lid_current(current_base, 1, current_lid, 1);
		return;
	}

	/* Estimate system power. */
	lid_system_power = charger_get_system_power() / 1000;

	/* Smooth system power, as it is very spiky */
	lid_system_power = smooth_value(prev_lid_system_power, lid_system_power,
					db_policy.lid_system_power_smooth);
	prev_lid_system_power = lid_system_power;

	/*
	 * TODO(b:71881017): Smoothing the battery power isn't necessarily a
	 * good idea: if the system takes up too much power, we may reduce the
	 * estimate power too quickly, leading to oscillations when the system
	 * power goes down. Instead, we should probably estimate the current
	 * based on remaining capacity.
	 */
	/* Estimate lid battery power. */
	if (!(curr.batt.flags &
	      (BATT_FLAG_BAD_VOLTAGE | BATT_FLAG_BAD_CURRENT)))
		lid_battery_power =
			curr.batt.current * curr.batt.voltage / 1000;
	if (lid_battery_power < prev_lid_battery_power)
		lid_battery_power =
			smooth_value(prev_lid_battery_power, lid_battery_power,
				     db_policy.battery_power_smooth);
	if (!(curr.batt.flags &
	      (BATT_FLAG_BAD_DESIRED_VOLTAGE | BATT_FLAG_BAD_DESIRED_CURRENT)))
		lid_battery_power_max = curr.batt.desired_current *
					curr.batt.desired_voltage / 1000;

	lid_battery_power = MIN(lid_battery_power, lid_battery_power_max);

	/* Estimate base battery power. */
	if (!(base_bd->flags & EC_BATT_FLAG_INVALID_DATA)) {
		base_battery_power = base_bd->actual_current *
				     base_bd->actual_voltage / 1000;
		base_battery_power_max = base_bd->desired_current *
					 base_bd->desired_voltage / 1000;
	}
	if (base_battery_power < prev_base_battery_power)
		base_battery_power = smooth_value(
			prev_base_battery_power, base_battery_power,
			db_policy.battery_power_smooth);
	base_battery_power = MIN(base_battery_power, base_battery_power_max);

	if (debugging) {
		CPRINTF("%s:\n", __func__);
		CPRINTF("total power: %d\n", total_power);
		CPRINTF("base battery power: %d (%d)\n", base_battery_power,
			base_battery_power_max);
		CPRINTF("lid system power: %d\n", lid_system_power);
		CPRINTF("lid battery power: %d\n", lid_battery_power);
		CPRINTF("percent base/lid: %d%% %d%%\n", charge_base,
			charge_lid);
	}

	prev_lid_battery_power = lid_battery_power;
	prev_base_battery_power = base_battery_power;

	if (total_power > 0) { /* Charging */
		/* Allocate system power */
		CHG_ALLOCATE(power_base, total_power,
			     db_policy.min_base_system_power);
		CHG_ALLOCATE(power_lid, total_power, lid_system_power);

		/* Allocate lid, then base battery power */
		lid_battery_power = add_margin(
			lid_battery_power, db_policy.margin_lid_battery_power);
		CHG_ALLOCATE(power_lid, total_power, lid_battery_power);

		base_battery_power =
			add_margin(base_battery_power,
				   db_policy.margin_base_battery_power);
		CHG_ALLOCATE(power_base, total_power, base_battery_power);

		/* Give everything else to the lid. */
		CHG_ALLOCATE(power_lid, total_power, total_power);
		if (debugging)
			CPRINTF("power: base %d mW / lid %d mW\n", power_base,
				power_lid);

		current_base = 1000 * power_base / curr.input_voltage;
		current_lid = 1000 * power_lid / curr.input_voltage;

		if (current_base > db_policy.max_lid_to_base_current) {
			current_lid += (current_base -
					db_policy.max_lid_to_base_current);
			current_base = db_policy.max_lid_to_base_current;
		}

		if (debugging)
			CPRINTF("current: base %d mA / lid %d mA\n",
				current_base, current_lid);

		set_base_lid_current(current_base, 1, current_lid, 1);
	} else { /* Discharging */
	}

	if (debugging)
		CPRINTF("====\n");
#endif /* CONFIG_EC_EC_COMM_BATTERY_CLIENT */
}

/* Update base battery information */
static void base_update_battery_info(void)
{
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	struct ec_response_battery_dynamic_info *const bd =
		&battery_dynamic[BATT_IDX_BASE];

	base_connected = board_is_base_connected();

	if (!base_connected) {
		const int invalid_flags = EC_BATT_FLAG_INVALID_DATA;
		/* Invalidate static/dynamic information */
		if (bd->flags != invalid_flags) {
			bd->flags = invalid_flags;

			host_set_single_event(EC_HOST_EVENT_BATTERY);
			host_set_single_event(EC_HOST_EVENT_BATTERY_STATUS);
		}
		charge_base = -1;
		base_responsive = 0;
		prev_current_base = 0;
		prev_allow_charge_base = 0;
	} else if (base_responsive) {
		int old_flags = bd->flags;
		int flags_changed;
		int old_full_capacity = bd->full_capacity;

		ec_ec_client_base_get_dynamic_info();
		flags_changed = (old_flags != bd->flags);
		/* Fetch static information when flags change. */
		if (flags_changed)
			ec_ec_client_base_get_static_info();

		battery_memmap_refresh(BATT_IDX_BASE);

		/* Newly connected battery, or change in capacity. */
		if (old_flags & EC_BATT_FLAG_INVALID_DATA ||
		    ((old_flags & EC_BATT_FLAG_BATT_PRESENT) !=
		     (bd->flags & EC_BATT_FLAG_BATT_PRESENT)) ||
		    old_full_capacity != bd->full_capacity)
			host_set_single_event(EC_HOST_EVENT_BATTERY);

		if (flags_changed)
			host_set_single_event(EC_HOST_EVENT_BATTERY_STATUS);

		/* Update charge_base */
		if (bd->flags & (BATT_FLAG_BAD_FULL_CAPACITY |
				 BATT_FLAG_BAD_REMAINING_CAPACITY))
			charge_base = -1;
		else if (bd->full_capacity > 0)
			charge_base = 100 * bd->remaining_capacity /
				      bd->full_capacity;
		else
			charge_base = 0;
	}
#endif /* CONFIG_EC_EC_COMM_BATTERY_CLIENT */
}

static const char *const state_list[] = { "idle", "discharge", "charge",
					  "precharge" };
BUILD_ASSERT(ARRAY_SIZE(state_list) == NUM_STATES_V2);
static const char *const batt_pres[] = {
	"NO",
	"YES",
	"NOT_SURE",
};

const char *mode_text[] = EC_CHARGE_MODE_TEXT;
BUILD_ASSERT(ARRAY_SIZE(mode_text) == CHARGE_CONTROL_COUNT);

static void dump_charge_state(void)
{
#define DUMP(FLD, FMT) ccprintf(#FLD " = " FMT "\n", curr.FLD)
#define DUMP_CHG(FLD, FMT) ccprintf("\t" #FLD " = " FMT "\n", curr.chg.FLD)
#define DUMP_BATT(FLD, FMT) ccprintf("\t" #FLD " = " FMT "\n", curr.batt.FLD)
#define DUMP_OCPC(FLD, FMT) ccprintf("\t" #FLD " = " FMT "\n", curr.ocpc.FLD)

	enum ec_charge_control_mode cmode = get_chg_ctrl_mode();

	ccprintf("state = %s\n", state_list[curr.state]);
	DUMP(ac, "%d");
	DUMP(batt_is_charging, "%d");
	ccprintf("chg.*:\n");
	DUMP_CHG(voltage, "%dmV");
	DUMP_CHG(current, "%dmA");
	DUMP_CHG(input_current, "%dmA");
	DUMP_CHG(status, "0x%x");
	DUMP_CHG(option, "0x%x");
	DUMP_CHG(flags, "0x%x");
	cflush();
	ccprintf("batt.*:\n");
	ccprintf("\ttemperature = %dC\n",
		 DECI_KELVIN_TO_CELSIUS(curr.batt.temperature));
	DUMP_BATT(state_of_charge, "%d%%");
	DUMP_BATT(voltage, "%dmV");
	DUMP_BATT(current, "%dmA");
	DUMP_BATT(desired_voltage, "%dmV");
	DUMP_BATT(desired_current, "%dmA");
	DUMP_BATT(flags, "0x%x");
	DUMP_BATT(remaining_capacity, "%dmAh");
	DUMP_BATT(full_capacity, "%dmAh");
	ccprintf("\tis_present = %s\n", batt_pres[curr.batt.is_present]);
	cflush();
#ifdef CONFIG_OCPC
	ccprintf("ocpc.*:\n");
	DUMP_OCPC(active_chg_chip, "%d");
	DUMP_OCPC(combined_rsys_rbatt_mo, "%dmOhm");
	if ((curr.ocpc.active_chg_chip != -1) &&
	    !(curr.ocpc.chg_flags[curr.ocpc.active_chg_chip] &
	      OCPC_NO_ISYS_MEAS_CAP)) {
		DUMP_OCPC(rbatt_mo, "%dmOhm");
		DUMP_OCPC(rsys_mo, "%dmOhm");
		DUMP_OCPC(isys_ma, "%dmA");
	}
	DUMP_OCPC(vsys_aux_mv, "%dmV");
	DUMP_OCPC(vsys_mv, "%dmV");
	DUMP_OCPC(primary_vbus_mv, "%dmV");
	DUMP_OCPC(primary_ibus_ma, "%dmA");
	DUMP_OCPC(secondary_vbus_mv, "%dmV");
	DUMP_OCPC(secondary_ibus_ma, "%dmA");
	DUMP_OCPC(last_error, "%d");
	DUMP_OCPC(integral, "%d");
	DUMP_OCPC(last_vsys, "%dmV");
	cflush();
#endif /* CONFIG_OCPC */
	DUMP(requested_voltage, "%dmV");
	DUMP(requested_current, "%dmA");
#ifdef CONFIG_CHARGER_OTG
	DUMP(output_current, "%dmA");
#endif
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	DUMP(input_voltage, "%dmV");
#endif
	ccprintf("chg_ctl_mode = %s (%d)\n",
		 cmode < CHARGE_CONTROL_COUNT ? mode_text[cmode] : "UNDEF",
		 cmode);
	ccprintf("manual_voltage = %d\n", manual_voltage);
	ccprintf("manual_current = %d\n", manual_current);
	ccprintf("user_current_limit = %dmA\n", user_current_limit);
	ccprintf("battery_seems_dead = %d\n", battery_seems_dead);
	ccprintf("battery_seems_disconnected = %d\n",
		 battery_seems_disconnected);
	ccprintf("battery_was_removed = %d\n", battery_was_removed);
	ccprintf("debug output = %s\n", debugging ? "on" : "off");
	ccprintf("Battery sustainer = %s (%d%% ~ %d%%)\n",
		 battery_sustainer_enabled() ? "on" : "off", sustain_soc.lower,
		 sustain_soc.upper);
#undef DUMP
}

bool charging_progress_displayed(void)
{
	bool rv = is_charging_progress_displayed;

	is_charging_progress_displayed = false;
	return rv;
}

static void show_charging_progress(void)
{
	int rv = 0, minutes, to_full, chgnum = 0;
	int dsoc;

	if (IS_ENABLED(TEST_BUILD))
		is_charging_progress_displayed = true;
#ifdef CONFIG_BATTERY_SMART
	/*
	 * Predicted remaining battery capacity based on AverageCurrent().
	 * 65535 = Battery is not being discharged.
	 */
	if (!battery_time_to_empty(&minutes) && minutes != 65535)
		to_full = 0;
	/*
	 * Predicted time-to-full charge based on AverageCurrent().
	 * 65535 = Battery is not being discharged.
	 */
	else if (!battery_time_to_full(&minutes) && minutes != 65535)
		to_full = 1;
	/*
	 * If both time to empty and time to full have invalid data, consider
	 * measured current from the coulomb counter and ac present status to
	 * decide whether battery is about to full or empty.
	 */
	else {
		to_full = curr.batt_is_charging;
		rv = EC_ERROR_UNKNOWN;
	}
#else
	if (!curr.batt_is_charging) {
		rv = battery_time_to_empty(&minutes);
		to_full = 0;
	} else {
		rv = battery_time_to_full(&minutes);
		to_full = 1;
	}
#endif

	dsoc = charge_get_display_charge();
	if (rv)
		CPRINTS("Battery %d%% (Display %d.%d %%) / ??h:?? %s%s",
			curr.batt.state_of_charge, dsoc / 10, dsoc % 10,
			to_full ? "to full" : "to empty",
			is_full ? ", not accepting current" : "");
	else
		CPRINTS("Battery %d%% (Display %d.%d %%) / %dh:%d %s%s",
			curr.batt.state_of_charge, dsoc / 10, dsoc % 10,
			minutes / 60, minutes % 60,
			to_full ? "to full" : "to empty",
			is_full ? ", not accepting current" : "");

#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	CPRINTS("Base battery %d%%", charge_base);
#endif

	if (debugging) {
		ccprintf("battery:\n");
		print_battery_debug();
		ccprintf("charger:\n");
		if (IS_ENABLED(CONFIG_OCPC))
			chgnum = charge_get_active_chg_chip();
		print_charger_debug(chgnum);
		ccprintf("chg:\n");
		dump_charge_state();
	}
}

/* Calculate if battery is full based on whether it is accepting charge */
test_mockable int calc_is_full(void)
{
	static int ret;

	/* If bad state of charge reading, return last value */
	if (curr.batt.flags & BATT_FLAG_BAD_STATE_OF_CHARGE ||
	    curr.batt.state_of_charge > 100)
		return ret;
	/*
	 * Battery is full when SoC is above 90% and battery desired current
	 * is 0. This is necessary because some batteries stop charging when
	 * the SoC still reports <100%, so we need to check desired current
	 * to know if it is actually full.
	 */
	ret = (curr.batt.state_of_charge >= 90 &&
	       curr.batt.desired_current == 0);
	return ret;
}

__overridable int board_should_charger_bypass(void)
{
	return false;
}

/*
 * Ask the charger for some voltage and current. If either value is 0,
 * charging is disabled; otherwise it's enabled. Negative values are ignored.
 */
static int charge_request(int voltage, int current)
{
	int r1 = EC_SUCCESS, r2 = EC_SUCCESS, r3 = EC_SUCCESS, r4 = EC_SUCCESS;
	static int prev_volt, prev_curr;
	bool should_bypass;

	if (!voltage || !current) {
#ifdef CONFIG_CHARGER_NARROW_VDC
		current = 0;
		/*
		 * With NVDC charger, keep VSYS voltage higher than battery,
		 * otherwise the BGATE FET body diode would conduct and
		 * discharge the battery.
		 */
		voltage = charger_closest_voltage(
			curr.batt.voltage + charger_get_info()->voltage_step);
		/* If the battery is full, request the max voltage. */
		if (is_full)
			voltage = battery_get_info()->voltage_max;
		/* And handle dead battery case */
		voltage = MAX(voltage, battery_get_info()->voltage_normal);
#else
		voltage = current = 0;
#endif
	}

	if (curr.ac) {
		if (prev_volt != voltage || prev_curr != current)
			CPRINTS("%s(%dmV, %dmA)", __func__, voltage, current);
	}

	/*
	 * Enable bypass mode if applicable. Transition from Bypass to Bypass +
	 * CHRG or backward is done after this call (by set_current & set_mode)
	 * thus not done here. Similarly, when bypass is disabled, transitioning
	 * from nvdc + chrg will be done separately.
	 */
	should_bypass = board_should_charger_bypass();
	if ((should_bypass && !(curr.chg.status & CHARGER_BYPASS_MODE)) ||
	    (!should_bypass && (curr.chg.status & CHARGER_BYPASS_MODE)))
		charger_enable_bypass_mode(0, should_bypass);

	/*
	 * Set current before voltage so that if we are just starting
	 * to charge, we allow some time (i2c delay) for charging circuit to
	 * start at a voltage just above battery voltage before jumping
	 * up. This helps avoid large current spikes when connecting
	 * battery.
	 */
	if (current >= 0) {
#ifdef CONFIG_OCPC
		/*
		 * For OCPC systems, don't unconditionally modify the primary
		 * charger IC's charge current.  It may be handled by the
		 * charger drivers directly.
		 */
		if (curr.ocpc.active_chg_chip == CHARGER_PRIMARY)
#endif
			r2 = charger_set_current(0, current);
	}
	if (r2 != EC_SUCCESS)
		charge_problem(PR_SET_CURRENT, r2);

	if (voltage >= 0)
		r1 = charger_set_voltage(0, voltage);
	if (r1 != EC_SUCCESS)
		charge_problem(PR_SET_VOLTAGE, r1);

#ifdef CONFIG_OCPC
	/*
	 * For OCPC systems, if the secondary charger is active, we need to
	 * configure that charge IC as well.  Note that if OCPC ever supports
	 * more than 2 charger ICs, we'll need to refactor things a bit.  The
	 * following check should be comparing against CHARGER_PRIMARY and
	 * config_secondary_charger should probably be config_auxiliary_charger
	 * and take the active chgnum as a parameter.
	 */
	if (curr.ocpc.active_chg_chip == CHARGER_SECONDARY) {
		if ((current >= 0) || (voltage >= 0))
			r3 = ocpc_config_secondary_charger(
				&curr.desired_input_current, &curr.ocpc,
				voltage, current);
		if (r3 != EC_SUCCESS)
			charge_problem(PR_CFG_SEC_CHG, r3);
	}
#endif /* CONFIG_OCPC */

	/*
	 * Set the charge inhibit bit when possible as it appears to save
	 * power in some cases (e.g. Nyan with BQ24735).
	 */
	if (voltage > 0 || current > 0)
		r4 = charger_set_mode(0);
	else
		r4 = charger_set_mode(CHARGE_FLAG_INHIBIT_CHARGE);
	if (r4 != EC_SUCCESS)
		charge_problem(PR_SET_MODE, r4);

	/*
	 * Only update if the request worked, so we'll keep trying on failures.
	 */
	if (r1 || r2)
		return r1 ? r1 : r2;
	if (IS_ENABLED(CONFIG_OCPC) && r3)
		return r3;

	if (IS_ENABLED(CONFIG_USB_PD_PREFER_MV) &&
	    (prev_volt != voltage || prev_curr != current))
		charge_reset_stable_current();

	prev_volt = voltage;
	prev_curr = current;

	return EC_SUCCESS;
}

void chgstate_set_manual_current(int curr_ma)
{
	if (curr_ma < 0)
		manual_current = -1;
	else
		manual_current = charger_closest_current(curr_ma);
}

void chgstate_set_manual_voltage(int volt_mv)
{
	manual_voltage = charger_closest_voltage(volt_mv);
}

/* Force charging off before the battery is full. */
static int set_chg_ctrl_mode(enum ec_charge_control_mode mode)
{
	bool discharge_on_ac = false;
	int current, voltage;
	int rv;

	current = manual_current;
	voltage = manual_voltage;

	if (mode >= CHARGE_CONTROL_COUNT)
		return EC_ERROR_INVAL;

	if (mode == CHARGE_CONTROL_NORMAL) {
		current = -1;
		voltage = -1;
	} else {
		/* Changing mode is only meaningful if AC is present. */
		if (!curr.ac)
			return EC_ERROR_NOT_POWERED;

		if (mode == CHARGE_CONTROL_DISCHARGE) {
			if (!IS_ENABLED(CONFIG_CHARGER_DISCHARGE_ON_AC))
				return EC_ERROR_UNIMPLEMENTED;
			discharge_on_ac = true;
		} else if (mode == CHARGE_CONTROL_IDLE) {
			current = 0;
			voltage = 0;
		}
	}

	if (IS_ENABLED(CONFIG_CHARGER_DISCHARGE_ON_AC)) {
		rv = charger_discharge_on_ac(discharge_on_ac);
		if (rv != EC_SUCCESS)
			return rv;
	}

	/* Commit all atomically */
	chg_ctl_mode = mode;
	manual_current = current;
	manual_voltage = voltage;

	return EC_SUCCESS;
}

static inline int battery_too_hot(int batt_temp_c)
{
	return (!(curr.batt.flags & BATT_FLAG_BAD_TEMPERATURE) &&
		(batt_temp_c > batt_info->discharging_max_c));
}

static inline int battery_too_cold_for_discharge(int batt_temp_c)
{
	return (!(curr.batt.flags & BATT_FLAG_BAD_TEMPERATURE) &&
		(batt_temp_c < batt_info->discharging_min_c));
}

__attribute__((weak)) uint8_t board_set_battery_level_shutdown(void)
{
	return BATTERY_LEVEL_SHUTDOWN;
}

/* True if we know the charge is too low, or we know the voltage is too low. */
static inline int battery_too_low(void)
{
	return ((!(curr.batt.flags & BATT_FLAG_BAD_STATE_OF_CHARGE) &&
		 curr.batt.state_of_charge < battery_level_shutdown) ||
		(!(curr.batt.flags & BATT_FLAG_BAD_VOLTAGE) &&
		 curr.batt.voltage <= batt_info->voltage_min));
}

__attribute__((weak)) enum critical_shutdown
board_critical_shutdown_check(struct charge_state_data *curr)
{
#ifdef CONFIG_BATTERY_CRITICAL_SHUTDOWN_CUT_OFF
	return CRITICAL_SHUTDOWN_CUTOFF;
#elif defined(CONFIG_HIBERNATE)
	return CRITICAL_SHUTDOWN_HIBERNATE;
#else
	return CRITICAL_SHUTDOWN_IGNORE;
#endif
}

static int is_battery_critical(void)
{
	int batt_temp_c = DECI_KELVIN_TO_CELSIUS(curr.batt.temperature);

	/*
	 * TODO(crosbug.com/p/27642): The thermal loop should watch the battery
	 * temp, so it can turn fans on.
	 */
	if (battery_too_hot(batt_temp_c)) {
		CPRINTS("Batt too hot: %dC", batt_temp_c);
		return 1;
	}

	/* Note: the battery may run on AC without discharging when too cold */
	if (!curr.ac && battery_too_cold_for_discharge(batt_temp_c)) {
		CPRINTS("Batt too cold: %dC", batt_temp_c);
		return 1;
	}

	if (battery_too_low() && !curr.batt_is_charging) {
		CPRINTS("Low battery: %d%%, %dmV", curr.batt.state_of_charge,
			curr.batt.voltage);
		return 1;
	}

	return 0;
}

/*
 * If the battery is at extremely low charge (and discharging) or extremely
 * high temperature, the EC will notify the AP and start a timer. If the
 * critical condition is not corrected before the timeout expires, the EC
 * will shut down the AP (if the AP is not already off) and then optionally
 * hibernate or cut off battery.
 */
static int shutdown_on_critical_battery(void)
{
	if (!is_battery_critical()) {
		/* Reset shutdown warning time */
		shutdown_target_time.val = 0;
		return 0;
	}

	if (!shutdown_target_time.val) {
		/* Start count down timer */
		CPRINTS("Start shutdown due to critical battery");
		shutdown_target_time.val =
			get_time().val + CRITICAL_BATTERY_SHUTDOWN_TIMEOUT_US;
#ifdef CONFIG_HOSTCMD_EVENTS
		if (!chipset_in_state(CHIPSET_STATE_ANY_OFF))
			host_set_single_event(EC_HOST_EVENT_BATTERY_SHUTDOWN);
#endif
		return 1;
	}

	if (!timestamp_expired(shutdown_target_time, 0))
		return 1;

	/* Timer has expired */
	if (chipset_in_or_transitioning_to_state(CHIPSET_STATE_ANY_OFF)) {
		switch (board_critical_shutdown_check(&curr)) {
		case CRITICAL_SHUTDOWN_HIBERNATE:
			if (IS_ENABLED(CONFIG_HIBERNATE)) {
				/*
				 * If the chipset is on its way down but not
				 * quite there yet, give it a little time to
				 * get there.
				 */
				if (!chipset_in_state(CHIPSET_STATE_ANY_OFF))
					sleep(1);
				CPRINTS("Hibernate due to critical battery");
				cflush();
				system_hibernate(0, 0);
			}
			break;
		case CRITICAL_SHUTDOWN_CUTOFF:
			/*
			 * Give the chipset just a sec to get to off if
			 * it's trying.
			 */
			if (!chipset_in_state(CHIPSET_STATE_ANY_OFF))
				sleep(1);
			CPRINTS("Cutoff due to critical battery");
			cflush();
			board_cut_off_battery();
			break;
		case CRITICAL_SHUTDOWN_IGNORE:
		default:
			break;
		}
	} else {
		/* Timeout waiting for AP to shut down, so kill it */
		CPRINTS("charge force shutdown due to critical battery");
		chipset_force_shutdown(CHIPSET_SHUTDOWN_BATTERY_CRIT);
	}

	return 1;
}

int battery_is_below_threshold(enum batt_threshold_type type, bool transitioned)
{
	int threshold;

	/* We can't tell what the current charge is. Assume it's okay. */
	if (curr.batt.flags & BATT_FLAG_BAD_STATE_OF_CHARGE)
		return 0;

	switch (type) {
	case BATT_THRESHOLD_TYPE_LOW:
		threshold = BATTERY_LEVEL_LOW;
		break;
	case BATT_THRESHOLD_TYPE_SHUTDOWN:
		threshold = CONFIG_BATT_HOST_SHUTDOWN_PERCENTAGE;
		break;
	default:
		return 0;
	}

	return curr.batt.state_of_charge <= threshold &&
	       (!transitioned || prev_charge > threshold);
}

/*
 * Send host events as the battery charge drops below certain thresholds.
 * We handle forced shutdown and other actions elsewhere; this is just for the
 * host events. We send these even if the AP is off, since the AP will read and
 * discard any events it doesn't care about the next time it wakes up.
 */
static void notify_host_of_low_battery_charge(void)
{
#ifdef CONFIG_HOSTCMD_EVENTS
	if (battery_is_below_threshold(BATT_THRESHOLD_TYPE_LOW, true))
		host_set_single_event(EC_HOST_EVENT_BATTERY_LOW);

	if (battery_is_below_threshold(BATT_THRESHOLD_TYPE_SHUTDOWN, true))
		host_set_single_event(EC_HOST_EVENT_BATTERY_CRITICAL);
#endif
}

static void set_charge_state(enum charge_state_v2 state)
{
	prev_state = curr.state;
	curr.state = state;
}

static void notify_host_of_low_battery_voltage(void)
{
#ifdef CONFIG_THROTTLE_AP_ON_BAT_VOLTAGE
	if ((curr.batt.flags & BATT_FLAG_BAD_VOLTAGE) ||
	    chipset_in_state(CHIPSET_STATE_ANY_OFF))
		return;

	if (!uvp_throttle_start_time.val &&
	    (curr.batt.voltage < BAT_LOW_VOLTAGE_THRESH)) {
		throttle_ap(THROTTLE_ON, THROTTLE_SOFT,
			    THROTTLE_SRC_BAT_VOLTAGE);
		uvp_throttle_start_time = get_time();
	} else if (uvp_throttle_start_time.val &&
		   (curr.batt.voltage <
		    BAT_LOW_VOLTAGE_THRESH + BAT_UVP_HYSTERESIS)) {
		/*
		 * Reset the timer when we are not sure if VBAT can stay
		 * above BAT_LOW_VOLTAGE_THRESH after we stop throttling.
		 */
		uvp_throttle_start_time = get_time();
	} else if (uvp_throttle_start_time.val &&
		   (get_time().val >
		    uvp_throttle_start_time.val + BAT_UVP_TIMEOUT_US)) {
		throttle_ap(THROTTLE_OFF, THROTTLE_SOFT,
			    THROTTLE_SRC_BAT_VOLTAGE);
		uvp_throttle_start_time.val = 0;
	}
#endif
}

static void notify_host_of_over_current(struct batt_params *batt)
{
#ifdef CONFIG_THROTTLE_AP_ON_BAT_DISCHG_CURRENT
	static timestamp_t ocp_throttle_start_time;

	if (batt->flags & BATT_FLAG_BAD_CURRENT)
		return;

	if ((!ocp_throttle_start_time.val &&
	     (batt->current < -BAT_MAX_DISCHG_CURRENT)) ||
	    (ocp_throttle_start_time.val &&
	     (batt->current < -BAT_MAX_DISCHG_CURRENT + BAT_OCP_HYSTERESIS))) {
		ocp_throttle_start_time = get_time();
		throttle_ap(THROTTLE_ON, THROTTLE_SOFT,
			    THROTTLE_SRC_BAT_DISCHG_CURRENT);
	} else if (ocp_throttle_start_time.val &&
		   (get_time().val >
		    ocp_throttle_start_time.val + BAT_OCP_TIMEOUT_US)) {
		/*
		 * Clear the timer and notify AP to stop throttling if
		 * we haven't seen over current for BAT_OCP_TIMEOUT_US.
		 */
		ocp_throttle_start_time.val = 0;
		throttle_ap(THROTTLE_OFF, THROTTLE_SOFT,
			    THROTTLE_SRC_BAT_DISCHG_CURRENT);
	}
#endif
}

const struct batt_params *charger_current_battery_params(void)
{
	return &curr.batt;
}

struct charge_state_data *charge_get_status(void)
{
	return &curr;
}

/* Determine if the battery is outside of allowable temperature range */
int battery_outside_charging_temperature(void)
{
	const struct battery_info *batt_info = battery_get_info();
	int batt_temp_c = DECI_KELVIN_TO_CELSIUS(curr.batt.temperature);
	int max_c, min_c;

	if (curr.batt.flags & BATT_FLAG_BAD_TEMPERATURE)
		return 0;

	if ((curr.batt.desired_voltage == 0) &&
	    (curr.batt.desired_current == 0)) {
		max_c = batt_info->start_charging_max_c;
		min_c = batt_info->start_charging_min_c;
	} else {
		max_c = batt_info->charging_max_c;
		min_c = batt_info->charging_min_c;
	}

	if ((batt_temp_c >= max_c) || (batt_temp_c <= min_c)) {
		return 1;
	}
	return 0;
}

static void sustain_battery_soc(void)
{
	enum ec_charge_control_mode mode = get_chg_ctrl_mode();
	int soc;
	int rv;

	/* If either AC or battery is not present, nothing to do. */
	if (!curr.ac || curr.batt.is_present != BP_YES ||
	    !battery_sustainer_enabled())
		return;

	soc = charge_get_display_charge() / 10;

	/*
	 * When lower < upper, the sustainer discharges using DISCHARGE. When
	 * lower == upper, the sustainer discharges using IDLE. The following
	 * switch statement handle both cases but in reality either DISCHARGE
	 * or IDLE is used but not both.
	 */
	switch (mode) {
	case CHARGE_CONTROL_NORMAL:
		/* Going up. Always DISCHARGE if the soc is above upper. */
		if (sustain_soc.lower == soc && soc == sustain_soc.upper) {
			mode = CHARGE_CONTROL_IDLE;
		} else if (sustain_soc.upper < soc) {
			mode = CHARGE_CONTROL_DISCHARGE;
		}
		break;
	case CHARGE_CONTROL_IDLE:
		/* Discharging naturally */
		if (soc < sustain_soc.lower)
			mode = CHARGE_CONTROL_NORMAL;
		break;
	case CHARGE_CONTROL_DISCHARGE:
		/* Discharging actively. */
		if (sustain_soc.lower == soc && soc == sustain_soc.upper) {
			mode = CHARGE_CONTROL_IDLE;
		} else if (soc < sustain_soc.lower) {
			mode = CHARGE_CONTROL_NORMAL;
		}
		break;
	default:
		return;
	}

	if (mode == get_chg_ctrl_mode())
		return;

	rv = set_chg_ctrl_mode(mode);
	CPRINTS("%s: %s control mode to %s", __func__,
		rv == EC_SUCCESS ? "Switched" : "Failed to switch",
		mode_text[mode]);
}

static void current_limit_battery_soc(void)
{
	if (user_current_limit != current_limit.value &&
	    charge_get_display_charge() / 10 >= current_limit.soc) {
		user_current_limit = current_limit.value;
		CPRINTS("Current limit %dmA applied", user_current_limit);
	}
}

/*****************************************************************************/
/* Hooks */
void charger_init(void)
{
	/* Initialize current state */
	memset(&curr, 0, sizeof(curr));
	curr.batt.is_present = BP_NOT_SURE;
	/* Manual voltage/current set to off */
	manual_voltage = -1;
	manual_current = -1;
	/*
	 * Other tasks read the params like state_of_charge at the beginning of
	 * their tasks. Make them ready first.
	 */
	battery_get_params(&curr.batt);

	battery_sustainer_disable();
}
DECLARE_HOOK(HOOK_INIT, charger_init, HOOK_PRIO_DEFAULT);

/* Wake up the task when something important happens */
static void charge_wakeup(void)
{
	task_wake(TASK_ID_CHARGER);
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, charge_wakeup, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_AC_CHANGE, charge_wakeup, HOOK_PRIO_DEFAULT);
DECLARE_HOOK(HOOK_POWER_SUPPLY_CHANGE, charge_wakeup, HOOK_PRIO_DEFAULT);

#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
/* Reset the base on S5->S0 transition. */
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, board_base_reset, HOOK_PRIO_DEFAULT);
#endif

#ifdef CONFIG_THROTTLE_AP_ON_BAT_VOLTAGE
static void bat_low_voltage_throttle_reset(void)
{
	uvp_throttle_start_time.val = 0;
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, bat_low_voltage_throttle_reset,
	     HOOK_PRIO_DEFAULT);
#endif

static int get_desired_input_current(enum battery_present batt_present,
				     const struct charger_info *const info)
{
	if (batt_present == BP_YES || system_is_locked() || base_connected) {
#ifdef CONFIG_CHARGE_MANAGER
		int ilim = charge_manager_get_charger_current();
		return ilim == CHARGE_CURRENT_UNINITIALIZED ?
			       CHARGE_CURRENT_UNINITIALIZED :
			       MAX(CONFIG_CHARGER_DEFAULT_CURRENT_LIMIT, ilim);
#else
		return CONFIG_CHARGER_DEFAULT_CURRENT_LIMIT;
#endif
	} else {
#ifdef CONFIG_USB_POWER_DELIVERY
		return MIN(PD_MAX_CURRENT_MA, info->input_current_max);
#else
		return info->input_current_max;
#endif
	}
}

static void wakeup_battery(int *need_static)
{
	if (battery_seems_dead || battery_is_cut_off()) {
		/* It's dead, do nothing */
		set_charge_state(ST_IDLE);
		curr.requested_voltage = 0;
		curr.requested_current = 0;
	} else if (curr.state == ST_PRECHARGE &&
		   (get_time().val >
		    precharge_start_time.val + PRECHARGE_TIMEOUT_US)) {
		/* We've tried long enough, give up */
		CPRINTS("battery seems to be dead");
		battery_seems_dead = 1;
		set_charge_state(ST_IDLE);
		curr.requested_voltage = 0;
		curr.requested_current = 0;
	} else {
		/* See if we can wake it up */
		if (curr.state != ST_PRECHARGE) {
			CPRINTS("try to wake battery");
			precharge_start_time = get_time();
			*need_static = 1;
		}
		set_charge_state(ST_PRECHARGE);
		curr.requested_voltage = batt_info->voltage_max;
		curr.requested_current = batt_info->precharge_current;
	}
}

__test_only enum charge_state_v2 charge_get_state_v2(void)
{
	return curr.state;
}

static void deep_charge_battery(int *need_static)
{
	if ((curr.state == ST_IDLE) &&
	    (curr.batt.flags & BATT_FLAG_DEEP_CHARGE)) {
		/* Deep charge time out , do nothing */
		curr.requested_voltage = 0;
		curr.requested_current = 0;
	} else if (curr.state == ST_PRECHARGE &&
		   (get_time().val >
		    precharge_start_time.val +
			    CONFIG_BATTERY_LOW_VOLTAGE_TIMEOUT)) {
		/* We've tried long enough, give up */
		CPRINTS("Precharge for low voltage timed out");
		set_charge_state(ST_IDLE);
		curr.requested_voltage = 0;
		curr.requested_current = 0;
	} else {
		/* See if we can wake it up */
		if (curr.state != ST_PRECHARGE) {
			CPRINTS("Start precharge for low voltage");
			precharge_start_time = get_time();
			*need_static = 1;
		}
		set_charge_state(ST_PRECHARGE);
		curr.requested_voltage = batt_info->voltage_max;
		curr.requested_current = batt_info->precharge_current;
		curr.batt.flags |= BATT_FLAG_DEEP_CHARGE;
	}
}

static void revive_battery(int *need_static)
{
	if (IS_ENABLED(CONFIG_BATTERY_REQUESTS_NIL_WHEN_DEAD) &&
	    curr.requested_voltage == 0 && curr.requested_current == 0 &&
	    curr.batt.state_of_charge == 0) {
		/*
		 * Battery is dead, give precharge current
		 * TODO (crosbug.com/p/29467): remove this workaround
		 * for dead battery that requests no voltage/current
		 */
		curr.requested_voltage = batt_info->voltage_max;
		curr.requested_current = batt_info->precharge_current;
	} else if (IS_ENABLED(CONFIG_BATTERY_REVIVE_DISCONNECT) &&
		   curr.requested_voltage == 0 && curr.requested_current == 0 &&
		   battery_seems_disconnected) {
		/*
		 * Battery is in disconnect state. Apply a
		 * current to kick it out of this state.
		 */
		CPRINTS("found battery in disconnect state");
		curr.requested_voltage = batt_info->voltage_max;
		curr.requested_current = batt_info->precharge_current;
	} else if (curr.state == ST_PRECHARGE || battery_seems_dead ||
		   battery_was_removed) {
		CPRINTS("battery woke up");
		/* Update the battery-specific values */
		batt_info = battery_get_info();
		*need_static = 1;
	}

	battery_seems_dead = battery_was_removed = 0;
}

/* Set up the initial state of the charger task */
static void charger_setup(const struct charger_info *info)
{
	/* Get the battery-specific values */
	batt_info = battery_get_info();

	prev_ac = prev_charge = prev_disp_charge = -1;
	chg_ctl_mode = CHARGE_CONTROL_NORMAL;
	shutdown_target_time.val = 0UL;
	battery_seems_dead = 0;
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	base_responsive = 0;
	curr.input_voltage = CHARGE_VOLTAGE_UNINITIALIZED;
	battery_dynamic[BATT_IDX_BASE].flags = EC_BATT_FLAG_INVALID_DATA;
	charge_base = -1;
#endif
#ifdef CONFIG_OCPC
	ocpc_init(&curr.ocpc);
	charge_set_active_chg_chip(CHARGE_PORT_NONE);
#endif /* CONFIG_OCPC */

	/*
	 * If system is not locked and we don't have a battery to live on,
	 * then use max input current limit so that we can pull as much power
	 * as needed.
	 */
	prev_bp = BP_NOT_INIT;
	curr.desired_input_current =
		get_desired_input_current(curr.batt.is_present, info);

	if (IS_ENABLED(CONFIG_USB_PD_PREFER_MV)) {
		/* init battery desired power */
		desired_mw =
			curr.batt.desired_current * curr.batt.desired_voltage;
		/*
		 * Battery charging current needs time to be stable when a
		 * new charge happens. Start the timer so we can evaluate the
		 * stable current when timeout.
		 */
		charge_reset_stable_current();
	}

	battery_level_shutdown = board_set_battery_level_shutdown();
}

/* Check base external-power settings and react as needed */
static void base_check_extpower(void)
{
	/* This #ifdef protects access to vars that are otherwise unavailable */
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	/*
	 * When base is powering the system, make sure curr.ac stays 0.
	 * TODO(b:71723024): Fix extpower_is_present() in hardware instead.
	 */
	if (base_responsive && prev_current_base < 0)
		curr.ac = 0;

	/* System is off: if AC gets connected, reset the base. */
	if (chipset_in_state(CHIPSET_STATE_ANY_OFF) && !prev_ac && curr.ac)
		board_base_reset();
#endif
}

/* check for and handle any state-of-charge change with the battery */
void check_battery_change_soc(void)
{
	if ((!(curr.batt.flags & BATT_FLAG_BAD_STATE_OF_CHARGE) &&
	     curr.batt.state_of_charge != prev_charge) ||
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	    (charge_base != prev_charge_base) ||
#endif
	    (is_full != prev_full) || (curr.state != prev_state) ||
	    (charge_get_display_charge() != prev_disp_charge)) {
		show_charging_progress();
		prev_charge = curr.batt.state_of_charge;
		prev_disp_charge = charge_get_display_charge();
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
		prev_charge_base = charge_base;
#endif
		hook_notify(HOOK_BATTERY_SOC_CHANGE);
	}
}

/* We've noticed a change in AC presence, let the board know */
static void process_ac_change(const int chgnum)
{
	board_check_extpower();
	if (curr.ac) {
		/*
		 * Some chargers are unpowered when the AC is off, so we'll
		 * reinitialize it when AC comes back and set the input current
		 * limit. Try again if it fails.
		 */
		int rv = charger_post_init();

		if (rv != EC_SUCCESS) {
			charge_problem(PR_POST_INIT, rv);
		} else if (curr.desired_input_current !=
			   CHARGE_CURRENT_UNINITIALIZED) {
			rv = charger_set_input_current_limit(
				chgnum, curr.desired_input_current);
			if (rv != EC_SUCCESS)
				charge_problem(PR_SET_INPUT_CURR, rv);
		}

		if (rv == EC_SUCCESS)
			prev_ac = curr.ac;
	} else {
		/* Some things are only meaningful on AC */
		set_chg_ctrl_mode(CHARGE_CONTROL_NORMAL);
		battery_seems_dead = 0;
		prev_ac = curr.ac;

		/*
		 * b/187967523, we should clear charge current, otherwise it
		 * will affect typeC output. This should be ok for all chargers.
		 */
		charger_set_current(chgnum, 0);
	}
}

/* Handle a change in the battery-present state */
static void process_battery_present_change(const struct charger_info *info,
					   int chgnum)
{
	prev_bp = curr.batt.is_present;

	/* Update battery info due to change of battery */
	batt_info = battery_get_info();

	curr.desired_input_current = get_desired_input_current(prev_bp, info);
	if (curr.desired_input_current != CHARGE_CURRENT_UNINITIALIZED)
		charger_set_input_current_limit(chgnum,
						curr.desired_input_current);
	hook_notify(HOOK_BATTERY_SOC_CHANGE);
}

/* Decide on the charge state we are in */
static void decide_charge_state(int *need_staticp, int *battery_criticalp)
{
	/* If we *know* there's no battery, wait for one to appear. */
	if (curr.batt.is_present == BP_NO) {
		if (!curr.ac)
			CPRINTS("running with no battery and no AC");
		set_charge_state(ST_IDLE);
		curr.batt_is_charging = 0;
		battery_was_removed = 1;
		return;
	}

	/*
	 * Always check the disconnect state if the battery is present. This is
	 * because the battery disconnect state is one of the items used to
	 * decide whether or not to leave safe mode.
	 *
	 * Note: For our purposes, an unresponsive battery is considered to be
	 * disconnected
	 */
	battery_seems_disconnected = battery_get_disconnect_state() !=
				     BATTERY_NOT_DISCONNECTED;

	/*
	 * If we had trouble talking to the battery or the charger, we should
	 * probably do nothing for a bit, and if it doesn't get better then flag
	 * it as an error.
	 */
	if (curr.chg.flags & CHG_FLAG_BAD_ANY)
		charge_problem(PR_CHG_FLAGS, curr.chg.flags);
	if (curr.batt.flags & BATT_FLAG_BAD_ANY)
		charge_problem(PR_BATT_FLAGS, curr.batt.flags);

	/*
	 * If AC is present, check if input current is sufficient to actually
	 * charge battery.
	 */
	curr.batt_is_charging = curr.ac && (curr.batt.current >= 0);

	/* Don't let the battery hurt itself. */
	*battery_criticalp = shutdown_on_critical_battery();

	if (!curr.ac) {
		set_charge_state(ST_DISCHARGE);
		return;
	}

	/* Okay, we're on AC and we should have a battery. */

	/* Used for factory tests. */
	if (get_chg_ctrl_mode() != CHARGE_CONTROL_NORMAL) {
		set_charge_state(ST_IDLE);
		return;
	}

	/* If the battery is not responsive, try to wake it up. */
	if (!(curr.batt.flags & BATT_FLAG_RESPONSIVE)) {
		wakeup_battery(need_staticp);
		return;
	}

	/*
	 * When the battery voltage is lower than voltage_min,precharge first to
	 * protect the battery
	 */
	if (IS_ENABLED(CONFIG_BATTERY_LOW_VOLTAGE_PROTECTION)) {
		if (!(curr.batt.flags & BATT_FLAG_BAD_VOLTAGE) &&
		    curr.batt.voltage <= batt_info->voltage_min) {
			deep_charge_battery(need_staticp);
			return;
		}

		/*
		 * Finished deep charge before timeout. Clear the flag so that
		 * we can do deep charge again (when it's deeply discharged
		 * again).
		 */
		if ((curr.batt.flags & BATT_FLAG_DEEP_CHARGE))
			curr.batt.flags &= ~BATT_FLAG_DEEP_CHARGE;
	}
	/* The battery is responding. Yay. Try to use it. */

	revive_battery(need_staticp);

	set_charge_state(ST_CHARGE);
}

/* Main loop */
void charger_task(void *u)
{
	int sleep_usec;
	int battery_critical;
	int need_static = 1;
	const struct charger_info *const info = charger_get_info();
	int prev_plt_and_desired_mw;
	int chgnum = 0;

	/* Set up the task - note that charger_init() has already run. */
	charger_setup(info);

	while (1) {
		/* Let's see what's going on... */
		curr.ts = get_time();
		sleep_usec = 0;
		problems_exist = 0;
		battery_critical = 0;
		curr.ac = extpower_is_present();
		if (IS_ENABLED(CONFIG_EC_EC_COMM_BATTERY_CLIENT))
			base_check_extpower();

		if (curr.ac != prev_ac)
			process_ac_change(chgnum);

		if (IS_ENABLED(CONFIG_EC_EC_COMM_BATTERY_CLIENT))
			base_update_battery_info();

		charger_get_params(&curr.chg);
		battery_get_params(&curr.batt);
#ifdef CONFIG_OCPC
		if (curr.ac)
			ocpc_get_adcs(&curr.ocpc);
#endif /* CONFIG_OCPC */

		if (prev_bp != curr.batt.is_present) {
			process_battery_present_change(info, chgnum);
			need_static = 1;
		}

		battery_validate_params(&curr.batt);

		notify_host_of_over_current(&curr.batt);

		/* battery current stable now, saves the current. */
		if (IS_ENABLED(CONFIG_USB_PD_PREFER_MV) &&
		    get_time().val > stable_ts.val && curr.batt.current >= 0)
			stable_current = curr.batt.current;

		/*
		 * Now decide what we want to do about it. We'll normally just
		 * pass along whatever the battery wants to the charger. Note
		 * that if battery_get_params() can't get valid values from the
		 * battery it uses (0, 0), which is probably safer than blindly
		 * applying power to a battery we can't talk to.
		 */
		if (curr.batt.flags & (BATT_FLAG_BAD_DESIRED_VOLTAGE |
				       BATT_FLAG_BAD_DESIRED_CURRENT)) {
			curr.requested_voltage = 0;
			curr.requested_current = 0;
		} else {
			curr.requested_voltage = curr.batt.desired_voltage;
			curr.requested_current = curr.batt.desired_current;
		}

		decide_charge_state(&need_static, &battery_critical);

		if (IS_ENABLED(CONFIG_CHARGER_PROFILE_OVERRIDE) &&
		    get_chg_ctrl_mode() == CHARGE_CONTROL_NORMAL) {
			sleep_usec = charger_profile_override(&curr);
			if (sleep_usec < 0)
				charge_problem(PR_CUSTOM, sleep_usec);
		}

		if (IS_ENABLED(CONFIG_BATTERY_CHECK_CHARGE_TEMP_LIMITS) &&
		    battery_outside_charging_temperature()) {
			curr.requested_current = 0;
			curr.requested_voltage = 0;
			curr.batt.flags &= ~BATT_FLAG_WANT_CHARGE;
			if (curr.state != ST_DISCHARGE)
				curr.state = ST_IDLE;
		}

		if (IS_ENABLED(CONFIG_CHARGE_MANAGER) &&
		    curr.batt.state_of_charge >=
			    CONFIG_CHARGE_MANAGER_BAT_PCT_SAFE_MODE_EXIT &&
		    !battery_seems_disconnected)
			charge_manager_leave_safe_mode();

		/* Keep the AP informed */
		if (need_static)
			need_static = update_static_battery_info();
		/* Wait on the dynamic info until the static info is good. */
		if (!need_static)
			update_dynamic_battery_info();
		notify_host_of_low_battery_charge();
		notify_host_of_low_battery_voltage();

		/* And the EC console */
		is_full = calc_is_full();

		/* Run battery sustainer (no-op if not applicable). */
		sustain_battery_soc();

		/* Run battery soc check for setting the current limit. */
		current_limit_battery_soc();

		check_battery_change_soc();

		prev_full = is_full;

		/* Turn charger off if it's not needed */
		if (!IS_ENABLED(CONFIG_CHARGER_MAINTAIN_VBAT) &&
		    (curr.state == ST_IDLE || curr.state == ST_DISCHARGE)) {
			curr.requested_voltage = 0;
			curr.requested_current = 0;
		}

		/* Apply external limits */
		if (curr.requested_current > user_current_limit)
			curr.requested_current = user_current_limit;

		/* Round to valid values */
		curr.requested_voltage =
			charger_closest_voltage(curr.requested_voltage);
		curr.requested_current =
			charger_closest_current(curr.requested_current);

		/* Charger only accpets request when AC is on. */
		if (curr.ac) {
			/*
			 * Some batteries would wake up after cut-off if we keep
			 * charging it. Thus, we only charge when AC is on and
			 * battery is not cut off yet.
			 */
			if (battery_is_cut_off()) {
				curr.requested_voltage = 0;
				curr.requested_current = 0;
			}
			/*
			 * As a safety feature, some chargers will stop
			 * charging if we don't communicate with it frequently
			 * enough. In manual mode, we'll just tell it what it
			 * knows.
			 */
			else {
				if (manual_voltage != -1)
					curr.requested_voltage = manual_voltage;
				if (manual_current != -1)
					curr.requested_current = manual_current;
			}
		} else if (!IS_ENABLED(CONFIG_CHARGER_MAINTAIN_VBAT)) {
			curr.requested_voltage = charger_closest_voltage(
				curr.batt.voltage + info->voltage_step);
			curr.requested_current = -1;
			/*
			 * On EC-EC server, do not charge if curr.ac is 0: there
			 * might still be some external power available but we
			 * do not want to use it for charging.
			 */
			if (IS_ENABLED(CONFIG_EC_EC_COMM_BATTERY_SERVER))
				curr.requested_current = 0;
		}

		if (IS_ENABLED(CONFIG_EC_EC_COMM_BATTERY_CLIENT))
			base_charge_allocate_input_current_limit();
		else
			charge_request(curr.requested_voltage,
				       curr.requested_current);

		/* How long to sleep? */
		if (problems_exist)
			/* If there are errors, don't wait very long. */
			sleep_usec = CHARGE_POLL_PERIOD_SHORT;
		else if (sleep_usec <= 0) {
			/* default values depend on the state */
			if (!curr.ac && (curr.state == ST_IDLE ||
					 curr.state == ST_DISCHARGE)) {
#ifdef CONFIG_CHARGER_OTG
				int output_current = curr.output_current;
#else
				int output_current = 0;
#endif
				/*
				 * If AP is off and we do not provide power, we
				 * can sleep a long time.
				 */
				if (chipset_in_state(
					    CHIPSET_STATE_ANY_OFF |
					    CHIPSET_STATE_ANY_SUSPEND) &&
				    output_current == 0)
					sleep_usec =
						CHARGE_POLL_PERIOD_VERY_LONG;
				else
					/* Discharging, not too urgent */
					sleep_usec = CHARGE_POLL_PERIOD_LONG;
			} else {
				/* AC present, so pay closer attention */
				sleep_usec = CHARGE_POLL_PERIOD_CHARGE;
			}
		}

		if (IS_ENABLED(CONFIG_USB_PD_PREFER_MV)) {
			int is_pd_supply = charge_manager_get_supplier() ==
					   CHARGE_SUPPLIER_PD;
			int port = charge_manager_get_active_charge_port();
			int bat_spec_desired_mw = curr.batt.desired_current *
						  curr.batt.desired_voltage /
						  1000;

			/*
			 * save the previous plt_and_desired_mw, since it
			 * will be updated below
			 */
			prev_plt_and_desired_mw =
				charge_get_plt_plus_bat_desired_mw();

			/*
			 * Update desired power by the following rules:
			 * 1. If the battery is not charging with PD, we reset
			 * the desired_mw to the battery spec. The actual
			 * desired_mw will be evaluated when it starts charging
			 * with PD again.
			 * 2. If the battery SoC under battery's constant
			 * voltage percent (this is a rough value that can be
			 * applied to most batteries), the battery can fully
			 * sink the power, the desired power should be the
			 * same as the battery spec, and we don't need to use
			 * evaluated value stable_current.
			 * 3. If the battery SoC is above battery's constant
			 * voltage percent, the real battery desired charging
			 * power will decrease slowly and so does the charging
			 * current. We can evaluate the battery desired power
			 * by the product of stable_current and battery voltage.
			 */
			if (!is_pd_supply)
				desired_mw = bat_spec_desired_mw;
			else if (curr.batt.state_of_charge < pd_pref_config.cv)
				desired_mw = bat_spec_desired_mw;
			else if (stable_current != CHARGE_CURRENT_UNINITIALIZED)
				desired_mw = curr.batt.voltage *
					     stable_current / 1000;

			/* if the plt_and_desired_mw changes, re-evaluate PDO */
			if (is_pd_supply &&
			    prev_plt_and_desired_mw !=
				    charge_get_plt_plus_bat_desired_mw())
				pd_set_new_power_request(port);
		}

		/* Adjust for time spent in this loop */
		sleep_usec -= (int)(get_time().val - curr.ts.val);
		if (sleep_usec < CHARGE_MIN_SLEEP_USEC)
			sleep_usec = CHARGE_MIN_SLEEP_USEC;
		else if (sleep_usec > CHARGE_MAX_SLEEP_USEC)
			sleep_usec = CHARGE_MAX_SLEEP_USEC;

		/*
		 * If battery is critical, ensure that the sleep time is not
		 * very long since we might want to hibernate or cut-off
		 * battery sooner.
		 */
		if (battery_critical &&
		    (sleep_usec > CRITICAL_BATTERY_SHUTDOWN_TIMEOUT_US))
			sleep_usec = CRITICAL_BATTERY_SHUTDOWN_TIMEOUT_US;

		task_wait_event(sleep_usec);
	}
}

/*****************************************************************************/
/* Exported functions */

int charge_want_shutdown(void)
{
	return (curr.state == ST_DISCHARGE) &&
	       !(curr.batt.flags & BATT_FLAG_BAD_STATE_OF_CHARGE) &&
	       (curr.batt.state_of_charge < battery_level_shutdown);
}

#ifdef CONFIG_CHARGER_MIN_BAT_PCT_FOR_POWER_ON
test_export_static int charge_prevent_power_on_automatic_power_on = 1;
#endif

bool charge_prevent_power_on(bool power_button_pressed)
{
	int prevent_power_on = 0;
	struct batt_params params;
	struct batt_params *current_batt_params = &curr.batt;

	/* If battery params seem uninitialized then retrieve them */
	if (current_batt_params->is_present == BP_NOT_SURE) {
		battery_get_params(&params);
		current_batt_params = &params;
	}

#ifdef CONFIG_CHARGER_MIN_BAT_PCT_FOR_POWER_ON

	/*
	 * Remember that a power button was pressed, and assume subsequent
	 * power-ups are user-requested and non-automatic.
	 */
	if (power_button_pressed)
		charge_prevent_power_on_automatic_power_on = 0;
	/*
	 * Require a minimum battery level to power on and ensure that the
	 * battery can provide power to the system.
	 */
	if (current_batt_params->is_present != BP_YES ||
#ifdef CONFIG_BATTERY_MEASURE_IMBALANCE
	    (current_batt_params->flags & BATT_FLAG_IMBALANCED_CELL &&
	     current_batt_params->state_of_charge <
		     CONFIG_CHARGER_MIN_BAT_PCT_IMBALANCED_POWER_ON) ||
#endif
#ifdef CONFIG_BATTERY_REVIVE_DISCONNECT
	    battery_get_disconnect_state() != BATTERY_NOT_DISCONNECTED ||
#endif
	    current_batt_params->state_of_charge <
		    CONFIG_CHARGER_MIN_BAT_PCT_FOR_POWER_ON)
		prevent_power_on = 1;

#if defined(CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON) && \
	defined(CONFIG_CHARGE_MANAGER)
	/* However, we can power on if a sufficient charger is present. */
	if (prevent_power_on) {
		if (charge_manager_get_power_limit_uw() >=
		    CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON * 1000)
			prevent_power_on = 0;
#if defined(CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON_WITH_BATT) && \
	defined(CONFIG_CHARGER_MIN_BAT_PCT_FOR_POWER_ON_WITH_AC)
		else if (charge_manager_get_power_limit_uw() >=
				 CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON_WITH_BATT *
					 1000
#ifdef CONFIG_BATTERY_REVIVE_DISCONNECT
			 && battery_get_disconnect_state() ==
				    BATTERY_NOT_DISCONNECTED
#endif
			 && (current_batt_params->state_of_charge >=
			     CONFIG_CHARGER_MIN_BAT_PCT_FOR_POWER_ON_WITH_AC))
			prevent_power_on = 0;
#endif
	}
#endif /* CONFIG_CHARGE_MANAGER && CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON */

	/*
	 * Factory override: Always allow power on if WP is disabled,
	 * except when auto-power-on at EC startup and the battery
	 * is physically present.
	 */
	prevent_power_on &= (system_is_locked() ||
			     (charge_prevent_power_on_automatic_power_on
#ifdef CONFIG_BATTERY_HW_PRESENT_CUSTOM

			      && battery_hw_present() == BP_YES
#endif
			      ));
#endif /* CONFIG_CHARGER_MIN_BAT_PCT_FOR_POWER_ON */

#ifdef CONFIG_CHARGE_MANAGER
	/* Always prevent power on until charge current is initialized */
	if (extpower_is_present() && (charge_manager_get_charger_current() ==
				      CHARGE_CURRENT_UNINITIALIZED))
		prevent_power_on = 1;
#ifdef CONFIG_BATTERY_HW_PRESENT_CUSTOM
	/*
	 * If battery is NOT physically present then prevent power on until
	 * a sufficient charger is present.
	 */
	if (extpower_is_present() && battery_hw_present() == BP_NO
#ifdef CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON
	    && charge_manager_get_power_limit_uw() <
		       CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON * 1000
#endif /* CONFIG_CHARGER_MIN_POWER_MW_FOR_POWER_ON */
	)
		prevent_power_on = 1;
#endif /* CONFIG_BATTERY_HW_PRESENT_CUSTOM */
#endif /* CONFIG_CHARGE_MANAGER */

		/*
		 * Prevent power on if there is no battery nor ac power. This
		 * happens when the servo is powering the EC to flash it. Only
		 * include this logic for boards in initial bring up phase since
		 * this won't happen for released boards.
		 */
#ifdef CONFIG_SYSTEM_UNLOCKED
	if (!current_batt_params->is_present && !curr.ac)
		prevent_power_on = 1;
#endif /* CONFIG_SYSTEM_UNLOCKED */

	return prevent_power_on != 0;
}

static int battery_near_full(void)
{
	if (charge_get_percent() < BATTERY_LEVEL_NEAR_FULL)
		return 0;

#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	if (charge_base > -1 && charge_base < BATTERY_LEVEL_NEAR_FULL)
		return 0;
#endif

	return 1;
}

enum charge_state charge_get_state(void)
{
	uint32_t chflags;

	switch (curr.state) {
	case ST_IDLE:
		chflags = charge_get_flags();

		if (battery_seems_dead || curr.batt.is_present == BP_NO)
			return PWR_STATE_ERROR;

		if (chflags & CHARGE_FLAG_FORCE_IDLE)
			return PWR_STATE_FORCED_IDLE;
		else
			return PWR_STATE_IDLE;
	case ST_DISCHARGE:
#ifdef CONFIG_PWR_STATE_DISCHARGE_FULL
		if (battery_near_full())
			return PWR_STATE_DISCHARGE_FULL;
		else
#endif
			return PWR_STATE_DISCHARGE;
	case ST_CHARGE:
		/* The only difference here is what the LEDs display. */
		if (IS_ENABLED(CONFIG_CHARGE_MANAGER) &&
		    charge_manager_get_active_charge_port() == CHARGE_PORT_NONE)
			return PWR_STATE_DISCHARGE;
		else if (battery_near_full())
			return PWR_STATE_CHARGE_NEAR_FULL;
		else
			return PWR_STATE_CHARGE;
	case ST_PRECHARGE:
		chflags = charge_get_flags();

		/* we're in battery discovery mode */
		if (chflags & CHARGE_FLAG_FORCE_IDLE)
			return PWR_STATE_FORCED_IDLE;
		else
			return PWR_STATE_IDLE;
	default:
		/* Anything else can be considered an error for LED purposes */
		return PWR_STATE_ERROR;
	}
}

uint32_t charge_get_flags(void)
{
	uint32_t flags = 0;

	if (get_chg_ctrl_mode() != CHARGE_CONTROL_NORMAL)
		flags |= CHARGE_FLAG_FORCE_IDLE;
	if (curr.ac)
		flags |= CHARGE_FLAG_EXTERNAL_POWER;
	if (curr.batt.flags & BATT_FLAG_RESPONSIVE)
		flags |= CHARGE_FLAG_BATT_RESPONSIVE;

	return flags;
}

int charge_get_percent(void)
{
	/*
	 * Since there's no way to indicate an error to the caller, we'll just
	 * return the last known value. Even if we've never been able to talk
	 * to the battery, that'll be zero, which is probably as good as
	 * anything.
	 */
	return is_full ? 100 : curr.batt.state_of_charge;
}

test_mockable int charge_get_display_charge(void)
{
	return curr.batt.display_charge;
}

int charge_get_battery_temp(int idx, int *temp_ptr)
{
	if (curr.batt.flags & BATT_FLAG_BAD_TEMPERATURE)
		return EC_ERROR_UNKNOWN;

	/* Battery temp is 10ths of degrees K, temp wants degrees K */
	*temp_ptr = curr.batt.temperature / 10;
	return EC_SUCCESS;
}

__overridable int charge_is_consuming_full_input_current(void)
{
	int chg_pct = charge_get_percent();

	return chg_pct > 2 && chg_pct < 95;
}

#ifdef CONFIG_CHARGER_OTG
int charge_set_output_current_limit(int chgnum, int ma, int mv)
{
	int ret;
	int enable = ma > 0;

	if (enable) {
		ret = charger_set_otg_current_voltage(chgnum, ma, mv);
		if (ret != EC_SUCCESS)
			return ret;
	}

	ret = charger_enable_otg_power(chgnum, enable);
	if (ret != EC_SUCCESS)
		return ret;

	/* If we start/stop providing power, wake the charger task. */
	if ((curr.output_current == 0 && enable) ||
	    (curr.output_current > 0 && !enable))
		task_wake(TASK_ID_CHARGER);

	curr.output_current = ma;

	return EC_SUCCESS;
}
#endif

int charge_set_input_current_limit(int ma, int mv)
{
	__maybe_unused int chgnum = 0;

#ifdef CONFIG_CHARGER_INPUT_CURRENT_DERATE_PCT
	if (CONFIG_CHARGER_INPUT_CURRENT_DERATE_PCT != 0) {
		ma = (ma * (100 - CONFIG_CHARGER_INPUT_CURRENT_DERATE_PCT)) /
		     100;
	}
#endif
#ifdef CONFIG_CHARGER_MIN_INPUT_CURRENT_LIMIT
	if (CONFIG_CHARGER_MIN_INPUT_CURRENT_LIMIT > 0) {
		ma = MAX(ma, CONFIG_CHARGER_MIN_INPUT_CURRENT_LIMIT);
	}
#endif

	if (IS_ENABLED(CONFIG_OCPC))
		chgnum = charge_get_active_chg_chip();
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	curr.input_voltage = mv;
#endif
	/*
	 * If battery is not present, we are not locked, and base is not
	 * connected then allow system to pull as much input current as needed.
	 * Yes, we might overcurrent the charger but this is no worse than
	 * browning out due to insufficient input current.
	 */
	if (curr.batt.is_present != BP_YES && !system_is_locked() &&
	    !base_connected) {
		int prev_input = 0;

		charger_get_input_current_limit(chgnum, &prev_input);

#ifdef CONFIG_USB_POWER_DELIVERY
#if ((PD_MAX_POWER_MW * 1000) / PD_MAX_VOLTAGE_MV != PD_MAX_CURRENT_MA)
		/*
		 * If battery is not present, input current is set to
		 * PD_MAX_CURRENT_MA. If the input power set is greater than
		 * the maximum allowed system power, system might get damaged.
		 * Hence, limit the input current to meet maximum allowed
		 * input system power.
		 */

		if (mv > 0 &&
		    mv * curr.desired_input_current > PD_MAX_POWER_MW * 1000)
			ma = (PD_MAX_POWER_MW * 1000) / mv;
		/*
		 * If the active charger has already been initialized to at
		 * least this current level, nothing left to do.
		 */
		else if (prev_input >= ma)
			return EC_SUCCESS;
#else
		if (prev_input >= ma)
			return EC_SUCCESS;
#endif
			/*
			 * If the current needs lowered due to PD max power
			 * considerations, or needs raised for the selected
			 * active charger chip, fall through to set.
			 */
#endif /* CONFIG_USB_POWER_DELIVERY */
	}

#ifdef CONFIG_CHARGER_MAX_INPUT_CURRENT
	/* Limit input current limit to max limit for this board */
	ma = MIN(ma, CONFIG_CHARGER_MAX_INPUT_CURRENT);
#endif

	if (IS_ENABLED(CONFIG_CHARGE_MANAGER)) {
		int pd_current_uncapped =
			charge_manager_get_pd_current_uncapped();

		/*
		 * clamp the input current to not exceeded the PD's limitation.
		 */
		if (pd_current_uncapped != CHARGE_CURRENT_UNINITIALIZED &&
		    ma > pd_current_uncapped)
			ma = pd_current_uncapped;
	}

	curr.desired_input_current = ma;
#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
	/* Wake up charger task to allocate current between lid and base. */
	charge_wakeup();
	return EC_SUCCESS;
#else
	return charger_set_input_current_limit(chgnum, ma);
#endif
}

#ifdef CONFIG_OCPC
void charge_set_active_chg_chip(int idx)
{
	ASSERT(idx < (int)board_get_charger_chip_count());

	if (idx == curr.ocpc.active_chg_chip)
		return;

	CPRINTS("Act Chg: %d", idx);
	curr.ocpc.active_chg_chip = idx;
}
#endif /* CONFIG_OCPC */

int charge_get_active_chg_chip(void)
{
#ifdef CONFIG_OCPC
	return curr.ocpc.active_chg_chip;
#else
	return 0;
#endif
}

#ifdef CONFIG_USB_PD_PREFER_MV
bool charge_is_current_stable(void)
{
	return get_time().val >= stable_ts.val;
}

int charge_get_plt_plus_bat_desired_mw(void)
{
	/*
	 * Ideally, the system consuming power could be evaluated by
	 * "IBus * VBus - battery charging power". But in practice,
	 * most charger drivers don't implement IBUS ADC reading,
	 * so we use system PLT instead as an alterntaive approach.
	 */
	return pd_pref_config.plt_mw + desired_mw;
}

int charge_get_stable_current(void)
{
	return stable_current;
}

void charge_set_stable_current(int ma)
{
	stable_current = ma;
}

void charge_reset_stable_current_us(uint64_t us)
{
	timestamp_t now = get_time();

	if (stable_ts.val < now.val + us)
		stable_ts.val = now.val + us;

	stable_current = CHARGE_CURRENT_UNINITIALIZED;
}

void charge_reset_stable_current(void)
{
	/* it takes 8 to 10 seconds to stabilize battery current in practice */
	charge_reset_stable_current_us(10 * SECOND);
}
#endif

#ifdef CONFIG_OCPC
void trigger_ocpc_reset(void)
{
	ocpc_reset(&curr.ocpc);
}
#endif

/*****************************************************************************/
/* Host commands */

static enum ec_status
charge_command_charge_control(struct host_cmd_handler_args *args)
{
	const struct ec_params_charge_control *p = args->params;
	struct ec_response_charge_control *r = args->response;
	int rv;

	if (args->version >= 2) {
		if (p->cmd == EC_CHARGE_CONTROL_CMD_SET) {
			if (p->mode == CHARGE_CONTROL_NORMAL) {
				rv = battery_sustainer_set(
					p->sustain_soc.lower,
					p->sustain_soc.upper);
				if (rv == EC_RES_UNAVAILABLE)
					return EC_RES_UNAVAILABLE;
				if (rv)
					return EC_RES_INVALID_PARAM;
			} else {
				battery_sustainer_disable();
			}
		} else if (p->cmd == EC_CHARGE_CONTROL_CMD_GET) {
			r->mode = get_chg_ctrl_mode();
			r->sustain_soc.lower = sustain_soc.lower;
			r->sustain_soc.upper = sustain_soc.upper;
			args->response_size = sizeof(*r);
			return EC_RES_SUCCESS;
		} else {
			return EC_RES_INVALID_PARAM;
		}
	}

	rv = set_chg_ctrl_mode(p->mode);
	if (rv != EC_SUCCESS)
		return EC_RES_ERROR;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_CHARGE_CONTROL, charge_command_charge_control,
		     EC_VER_MASK(1) | EC_VER_MASK(2));

static enum ec_status
charge_command_current_limit(struct host_cmd_handler_args *args)
{
	if (args->version == 0) {
		const struct ec_params_current_limit *p = args->params;
		user_current_limit = p->limit;
		current_limit.value = p->limit;
	} else {
		const struct ec_params_current_limit_v1 *p = args->params;

		/* Check if battery state of charge param is within range */
		if (p->battery_soc > 100) {
			CPRINTS("Invalid battery_soc: %d", p->battery_soc);
			return EC_RES_INVALID_PARAM;
		}

		current_limit.value = p->limit;
		current_limit.soc = p->battery_soc;
	}

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_CHARGE_CURRENT_LIMIT, charge_command_current_limit,
		     EC_VER_MASK(0) | EC_VER_MASK(1));

/*
 * Expose charge/battery related state
 *
 * @param param command to get corresponding data
 * @param value the corresponding data
 * @return EC_SUCCESS or error
 */
static int charge_get_charge_state_debug(int param, uint32_t *value)
{
	switch (param) {
	case CS_PARAM_DEBUG_CTL_MODE:
		*value = get_chg_ctrl_mode();
		break;
	case CS_PARAM_DEBUG_MANUAL_CURRENT:
		*value = manual_current;
		break;
	case CS_PARAM_DEBUG_MANUAL_VOLTAGE:
		*value = manual_voltage;
		break;
	case CS_PARAM_DEBUG_SEEMS_DEAD:
		*value = battery_seems_dead;
		break;
	case CS_PARAM_DEBUG_SEEMS_DISCONNECTED:
		*value = battery_seems_disconnected;
		break;
	case CS_PARAM_DEBUG_BATT_REMOVED:
		*value = battery_was_removed;
		break;
	default:
		*value = 0;
		return EC_ERROR_INVAL;
	}

	return EC_SUCCESS;
}

static enum ec_status
charge_command_charge_state(struct host_cmd_handler_args *args)
{
	const struct ec_params_charge_state *in = args->params;
	struct ec_response_charge_state *out = args->response;
	uint32_t val;
	int rv = EC_RES_SUCCESS;
	int chgnum = 0;

	if (args->version > 0)
		chgnum = in->chgnum;

	switch (in->cmd) {
	case CHARGE_STATE_CMD_GET_STATE:
		out->get_state.ac = curr.ac;
		out->get_state.chg_voltage = curr.chg.voltage;
		out->get_state.chg_current = curr.chg.current;
		out->get_state.chg_input_current = curr.chg.input_current;
		out->get_state.batt_state_of_charge = curr.batt.state_of_charge;
		args->response_size = sizeof(out->get_state);
		break;

	case CHARGE_STATE_CMD_GET_PARAM:
		val = 0;
		if (IS_ENABLED(CONFIG_CHARGER_PROFILE_OVERRIDE) &&
		    in->get_param.param >= CS_PARAM_CUSTOM_PROFILE_MIN &&
		    in->get_param.param <= CS_PARAM_CUSTOM_PROFILE_MAX) {
			/* custom profile params */
			rv = charger_profile_override_get_param(
				in->get_param.param, &val);
		} else if (IS_ENABLED(CONFIG_CHARGE_STATE_DEBUG) &&
			   in->get_param.param >= CS_PARAM_DEBUG_MIN &&
			   in->get_param.param <= CS_PARAM_DEBUG_MAX) {
			/* debug params */
			rv = charge_get_charge_state_debug(in->get_param.param,
							   &val);
		} else {
			/* standard params */
			switch (in->get_param.param) {
			case CS_PARAM_CHG_VOLTAGE:
				val = curr.chg.voltage;
				break;
			case CS_PARAM_CHG_CURRENT:
				val = curr.chg.current;
				break;
			case CS_PARAM_CHG_INPUT_CURRENT:
				val = curr.chg.input_current;
				break;
			case CS_PARAM_CHG_STATUS:
				val = curr.chg.status;
				break;
			case CS_PARAM_CHG_OPTION:
				val = curr.chg.option;
				break;
			case CS_PARAM_LIMIT_POWER:
#ifdef CONFIG_CHARGER_LIMIT_POWER_THRESH_CHG_MW
				/*
				 * LIMIT_POWER status is based on battery level
				 * and external charger power.
				 */
				if ((curr.batt.is_present != BP_YES ||
				     curr.batt.state_of_charge <
					     CONFIG_CHARGER_LIMIT_POWER_THRESH_BAT_PCT) &&
				    charge_manager_get_power_limit_uw() <
					    CONFIG_CHARGER_LIMIT_POWER_THRESH_CHG_MW *
						    1000 &&
				    system_is_locked())
					val = 1;
				else
#endif
					val = 0;
				break;
			default:
				rv = EC_RES_INVALID_PARAM;
			}
		}

		/* got something */
		out->get_param.value = val;
		args->response_size = sizeof(out->get_param);
		break;

	case CHARGE_STATE_CMD_SET_PARAM:
		if (system_is_locked())
			return EC_RES_ACCESS_DENIED;

		val = in->set_param.value;
		if (IS_ENABLED(CONFIG_CHARGER_PROFILE_OVERRIDE) &&
		    in->set_param.param >= CS_PARAM_CUSTOM_PROFILE_MIN &&
		    in->set_param.param <= CS_PARAM_CUSTOM_PROFILE_MAX) {
			/* custom profile params */
			rv = charger_profile_override_set_param(
				in->set_param.param, val);
		} else {
			switch (in->set_param.param) {
			case CS_PARAM_CHG_VOLTAGE:
				chgstate_set_manual_voltage(val);
				break;
			case CS_PARAM_CHG_CURRENT:
				chgstate_set_manual_current(val);
				break;
			case CS_PARAM_CHG_INPUT_CURRENT:
				if (charger_set_input_current_limit(chgnum,
								    val))
					rv = EC_RES_ERROR;
				break;
			case CS_PARAM_CHG_STATUS:
			case CS_PARAM_LIMIT_POWER:
				/* Can't set this */
				rv = EC_RES_ACCESS_DENIED;
				break;
			case CS_PARAM_CHG_OPTION:
				if (charger_set_option(val))
					rv = EC_RES_ERROR;
				break;
			default:
				rv = EC_RES_INVALID_PARAM;
			}
		}
		break;

	default:
		CPRINTS("EC_CMD_CHARGE_STATE: bad cmd 0x%x", in->cmd);
		rv = EC_RES_INVALID_PARAM;
	}

	return rv;
}

DECLARE_HOST_COMMAND(EC_CMD_CHARGE_STATE, charge_command_charge_state,
		     EC_VER_MASK(0) | EC_VER_MASK(1));

/*****************************************************************************/
/* Console commands */

#ifdef CONFIG_CMD_PWR_AVG

static int command_pwr_avg(int argc, const char **argv)
{
	int avg_mv;
	int avg_ma;
	int avg_mw;

	if (argc != 1)
		return EC_ERROR_PARAM_COUNT;

	avg_mv = battery_get_avg_voltage();
	if (avg_mv < 0)
		return EC_ERROR_UNKNOWN;
	avg_ma = battery_get_avg_current();
	avg_mw = avg_mv * avg_ma / 1000;

	ccprintf("mv = %d\nma = %d\nmw = %d\n", avg_mv, avg_ma, avg_mw);
	return EC_SUCCESS;
}

DECLARE_CONSOLE_COMMAND(pwr_avg, command_pwr_avg, NULL,
			"Get 1 min power average");

#endif /* CONFIG_CMD_PWR_AVG */

static int command_chgstate(int argc, const char **argv)
{
	int rv;
	int val;
	char *e;

	if (argc > 1) {
		if (!strcasecmp(argv[1], "idle")) {
			if (argc <= 2)
				return EC_ERROR_PARAM_COUNT;
			if (!parse_bool(argv[2], &val))
				return EC_ERROR_PARAM2;
			rv = set_chg_ctrl_mode(val ? CHARGE_CONTROL_IDLE :
						     CHARGE_CONTROL_NORMAL);
			if (rv)
				return rv;
		} else if (!strcasecmp(argv[1], "discharge")) {
			if (argc <= 2)
				return EC_ERROR_PARAM_COUNT;
			if (!parse_bool(argv[2], &val))
				return EC_ERROR_PARAM2;
			rv = set_chg_ctrl_mode(val ? CHARGE_CONTROL_DISCHARGE :
						     CHARGE_CONTROL_NORMAL);
			if (rv)
				return rv;
		} else if (!strcasecmp(argv[1], "debug")) {
			if (argc <= 2)
				return EC_ERROR_PARAM_COUNT;
			if (!parse_bool(argv[2], &debugging))
				return EC_ERROR_PARAM2;
		} else if (!strcasecmp(argv[1], "sustain")) {
			int lower, upper;

			if (argc <= 3)
				return EC_ERROR_PARAM_COUNT;
			lower = strtoi(argv[2], &e, 0);
			if (*e)
				return EC_ERROR_PARAM2;
			upper = strtoi(argv[3], &e, 0);
			if (*e)
				return EC_ERROR_PARAM3;
			rv = battery_sustainer_set(lower, upper);
			if (rv)
				return EC_ERROR_INVAL;
		} else {
			return EC_ERROR_PARAM1;
		}
	}

	dump_charge_state();
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(chgstate, command_chgstate,
			"[idle|discharge|debug on|off]"
			"\n[sustain <lower> <upper>]",
			"Get/set charge state machine status");

#ifdef CONFIG_EC_EC_COMM_BATTERY_CLIENT
static int command_chgdualdebug(int argc, const char **argv)
{
	int val;
	char *e;

	if (argc > 1) {
		if (argv[1][0] == 'c') {
			if (argc <= 2)
				return EC_ERROR_PARAM_COUNT;

			if (!strcasecmp(argv[2], "auto")) {
				val = -1;
			} else {
				val = strtoi(argv[2], &e, 0);
				if (*e || val < 0)
					return EC_ERROR_PARAM2;
			}

			manual_ac_current_base = val;
			charge_wakeup();
		} else if (argv[1][0] == 'd') {
			if (argc <= 2)
				return EC_ERROR_PARAM_COUNT;

			if (!strcasecmp(argv[2], "auto")) {
				manual_noac_enabled = 0;
			} else {
				val = strtoi(argv[2], &e, 0);
				if (*e)
					return EC_ERROR_PARAM2;
				manual_noac_current_base = val;
				manual_noac_enabled = 1;
			}
			charge_wakeup();
		} else {
			return EC_ERROR_PARAM1;
		}
	} else {
		ccprintf("Base/Lid: %d%s/%d mA\n", prev_current_base,
			 prev_allow_charge_base ? "+" : "", prev_current_lid);
	}

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(chgdualdebug, command_chgdualdebug,
			"[charge (auto|<current>)|discharge (auto|<current>)]",
			"Manually control dual-battery charging algorithm.");
#endif
