/*
 * Copyright © 2026 libratbag contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"
#include <assert.h>
#include <errno.h>
#include <linux/input.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "libratbag-private.h"
#include "libratbag-hidraw.h"
#include "libratbag-util.h"

/* Device constants */
#define PULSAR_NUM_PROFILES		2
#define PULSAR_NUM_DPI_MODES		8
#define PULSAR_NUM_BUTTONS		6
#define PULSAR_NUM_LEDS			1

/* Packet format */
#define PULSAR_REPORT_ID		0x08
#define PULSAR_PACKET_SIZE		17

/* Commands */
#define PULSAR_CMD_STATUS		0x03
#define PULSAR_CMD_MEM_SET		0x07
#define PULSAR_CMD_MEM_GET		0x08
#define PULSAR_CMD_ACTIVE_PROFILE_GET	0x0E
#define PULSAR_CMD_ACTIVE_PROFILE_SET	0x0F

/* Settings memory addresses */
#define PULSAR_ADDR_POLLING_RATE	0x00
#define PULSAR_ADDR_DPI_MODE_COUNT	0x02
#define PULSAR_ADDR_ACTIVE_DPI_MODE	0x04
#define PULSAR_ADDR_DPI_BASE		0x0C
#define PULSAR_ADDR_DPI_COLOR_BASE	0x2C
#define PULSAR_ADDR_LED_EFFECT		0x4C
#define PULSAR_ADDR_LED_BRIGHTNESS	0x4E
#define PULSAR_ADDR_LED_BREATHE_SPEED	0x50
#define PULSAR_ADDR_LED_ENABLED		0x52
#define PULSAR_ADDR_BUTTON_BASE		0x60
#define PULSAR_ADDR_DEBOUNCE		0xA9
#define PULSAR_ADDR_ANGLE_SNAPPING	0xAF

/* Settings region size */
#define PULSAR_SETTINGS_SIZE		0xB9

/* DPI range */
#define PULSAR_DPI_MIN			50
#define PULSAR_DPI_MAX			26000

/* Button modes */
#define PULSAR_BTN_MODE_DISABLED	0x00
#define PULSAR_BTN_MODE_MOUSE		0x01
#define PULSAR_BTN_MODE_DPI		0x02

/* Mouse button param values */
#define PULSAR_MOUSE_LEFT		0x01
#define PULSAR_MOUSE_RIGHT		0x02
#define PULSAR_MOUSE_MIDDLE		0x04
#define PULSAR_MOUSE_BACK		0x08
#define PULSAR_MOUSE_FORWARD		0x10

/* DPI change param values */
#define PULSAR_DPI_CYCLE		0x01
#define PULSAR_DPI_UP			0x02
#define PULSAR_DPI_DOWN			0x03

/* Polling rate values */
#define PULSAR_RATE_1000		0x01
#define PULSAR_RATE_500			0x02
#define PULSAR_RATE_250			0x04
#define PULSAR_RATE_125			0x08

/* LED effect values */
#define PULSAR_LED_STEADY		0x01
#define PULSAR_LED_BREATHE		0x02

struct pulsar_data {
	uint8_t settings[PULSAR_SETTINGS_SIZE];
	unsigned int active_profile;
};

static uint8_t
pulsar_checksum(const uint8_t *buf, size_t len)
{
	unsigned int sum = 0;

	for (size_t i = 0; i < len; i++)
		sum += buf[i];

	return (0x55 - sum) & 0xFF;
}

static void
pulsar_build_packet(uint8_t *pkt, uint8_t cmd, uint32_t addr,
		    uint8_t length, const uint8_t *data, size_t data_len)
{
	memset(pkt, 0, PULSAR_PACKET_SIZE);
	pkt[0] = PULSAR_REPORT_ID;
	pkt[1] = cmd;
	pkt[2] = (addr >> 16) & 0xFF;
	pkt[3] = (addr >> 8) & 0xFF;
	pkt[4] = addr & 0xFF;
	pkt[5] = length;

	if (data && data_len > 0)
		memcpy(&pkt[6], data, min(data_len, 10u));

	pkt[16] = pulsar_checksum(pkt, 16);
}

static bool
pulsar_report_filter(uint8_t *buf, size_t len)
{
	return len >= 1 && buf[0] == PULSAR_REPORT_ID;
}

static int
pulsar_send_command(struct ratbag_device *device, uint8_t *pkt)
{
	int rc;

	rc = ratbag_hidraw_output_report(device, pkt, PULSAR_PACKET_SIZE);
	if (rc < 0)
		return rc;

	return 0;
}

static int
pulsar_read_response(struct ratbag_device *device, uint8_t *buf)
{
	return ratbag_hidraw_read_input_report(device, buf, PULSAR_PACKET_SIZE,
					       pulsar_report_filter);
}

static int
pulsar_transact(struct ratbag_device *device, uint8_t *pkt, uint8_t *resp)
{
	int rc;

	rc = pulsar_send_command(device, pkt);
	if (rc < 0)
		return rc;

	rc = pulsar_read_response(device, resp);
	if (rc < 0)
		return rc;

	return 0;
}

static int
pulsar_status_check(struct ratbag_device *device)
{
	uint8_t pkt[PULSAR_PACKET_SIZE];
	uint8_t resp[PULSAR_PACKET_SIZE];
	int rc;

	pulsar_build_packet(pkt, PULSAR_CMD_STATUS, 0, 0, NULL, 0);
	rc = pulsar_transact(device, pkt, resp);
	if (rc < 0)
		return rc;

	if (resp[6] != 0x01) {
		log_error(device->ratbag, "Pulsar: device not active (status byte = 0x%02x)\n",
			  resp[6]);
		return -ENODEV;
	}

	return 0;
}

static int
pulsar_mem_get(struct ratbag_device *device, uint32_t addr, uint8_t *out,
	       uint8_t length)
{
	uint8_t pkt[PULSAR_PACKET_SIZE];
	uint8_t resp[PULSAR_PACKET_SIZE];
	int rc;

	assert(length >= 1 && length <= 10);

	pulsar_build_packet(pkt, PULSAR_CMD_MEM_GET, addr, length, NULL, 0);
	rc = pulsar_transact(device, pkt, resp);
	if (rc < 0)
		return rc;

	memcpy(out, &resp[6], length);
	return 0;
}

static int
pulsar_mem_set(struct ratbag_device *device, uint32_t addr, const uint8_t *data,
	       uint8_t length)
{
	uint8_t pkt[PULSAR_PACKET_SIZE];
	uint8_t resp[PULSAR_PACKET_SIZE];

	assert(length >= 1 && length <= 10);

	pulsar_build_packet(pkt, PULSAR_CMD_MEM_SET, addr, length, data, length);
	return pulsar_transact(device, pkt, resp);
}

static int
pulsar_read_settings(struct ratbag_device *device, struct pulsar_data *drv_data)
{
	uint8_t chunk_size;
	int rc;

	for (uint32_t offset = 0; offset < PULSAR_SETTINGS_SIZE; offset += chunk_size) {
		chunk_size = min((uint32_t)10, PULSAR_SETTINGS_SIZE - offset);
		rc = pulsar_mem_get(device, offset, &drv_data->settings[offset],
				    chunk_size);
		if (rc < 0) {
			log_error(device->ratbag,
				  "Pulsar: failed to read settings at 0x%04x\n",
				  offset);
			return rc;
		}
	}

	return 0;
}

static int
pulsar_get_active_profile(struct ratbag_device *device, unsigned int *out)
{
	uint8_t pkt[PULSAR_PACKET_SIZE];
	uint8_t resp[PULSAR_PACKET_SIZE];
	int rc;

	pulsar_build_packet(pkt, PULSAR_CMD_ACTIVE_PROFILE_GET, 0, 0, NULL, 0);
	rc = pulsar_transact(device, pkt, resp);
	if (rc < 0)
		return rc;

	*out = resp[6];
	return 0;
}

static unsigned int
pulsar_decode_dpi(const uint8_t *bytes)
{
	unsigned int factor50 = bytes[0];
	uint8_t factor12800_byte = bytes[2];
	unsigned int factor12800 = 0;

	if (factor12800_byte == 0x44)
		factor12800 = 1;
	else if (factor12800_byte == 0x88)
		factor12800 = 2;

	return (factor50 + 1) * 50 + factor12800 * 12800;
}

static void
pulsar_encode_dpi(unsigned int dpi, uint8_t *bytes)
{
	unsigned int factor12800 = 0;
	unsigned int remainder;
	uint8_t factor50;

	if (dpi > 25600) {
		factor12800 = 2;
	} else if (dpi > 12800) {
		factor12800 = 1;
	}

	remainder = dpi - factor12800 * 12800;
	factor50 = (remainder / 50) - 1;

	bytes[0] = factor50;
	bytes[1] = factor50;

	switch (factor12800) {
	case 0: bytes[2] = 0x00; break;
	case 1: bytes[2] = 0x44; break;
	case 2: bytes[2] = 0x88; break;
	}
}

static unsigned int
pulsar_polling_rate_to_hz(uint8_t val)
{
	switch (val) {
	case PULSAR_RATE_1000: return 1000;
	case PULSAR_RATE_500:  return 500;
	case PULSAR_RATE_250:  return 250;
	case PULSAR_RATE_125:  return 125;
	// TODO 4k dongle
	default: return 1000;
	}
}

static uint8_t
pulsar_hz_to_polling_rate(unsigned int hz)
{
	switch (hz) {
	case 1000: return PULSAR_RATE_1000;
	case 500:  return PULSAR_RATE_500;
	case 250:  return PULSAR_RATE_250;
	case 125:  return PULSAR_RATE_125;
	// TODO 4k dongle
	default:   return PULSAR_RATE_1000;
	}
}

struct pulsar_button_mapping {
	uint8_t mode;
	uint8_t param1;
	struct ratbag_button_action action;
};

static const struct pulsar_button_mapping pulsar_button_map[] = {
	{ PULSAR_BTN_MODE_DISABLED, 0x00, BUTTON_ACTION_NONE },
	{ PULSAR_BTN_MODE_MOUSE, PULSAR_MOUSE_LEFT, BUTTON_ACTION_BUTTON(1) },
	{ PULSAR_BTN_MODE_MOUSE, PULSAR_MOUSE_RIGHT, BUTTON_ACTION_BUTTON(2) },
	{ PULSAR_BTN_MODE_MOUSE, PULSAR_MOUSE_MIDDLE, BUTTON_ACTION_BUTTON(3) },
	{ PULSAR_BTN_MODE_MOUSE, PULSAR_MOUSE_BACK, BUTTON_ACTION_BUTTON(4) },
	{ PULSAR_BTN_MODE_MOUSE, PULSAR_MOUSE_FORWARD, BUTTON_ACTION_BUTTON(5) },
	{ PULSAR_BTN_MODE_DPI, PULSAR_DPI_CYCLE, BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_CYCLE_UP) },
	{ PULSAR_BTN_MODE_DPI, PULSAR_DPI_UP, BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_UP) },
	{ PULSAR_BTN_MODE_DPI, PULSAR_DPI_DOWN, BUTTON_ACTION_SPECIAL(RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_DOWN) },
};

static const struct ratbag_button_action *
pulsar_raw_to_action(uint8_t mode, uint8_t param1)
{
	for (size_t i = 0; i < ARRAY_LENGTH(pulsar_button_map); i++) {
		if (pulsar_button_map[i].mode == mode &&
		    pulsar_button_map[i].param1 == param1)
			return &pulsar_button_map[i].action;
	}

	return NULL;
}

static bool
pulsar_action_to_raw(const struct ratbag_button_action *action,
		     uint8_t *mode, uint8_t *param1)
{
	for (size_t i = 0; i < ARRAY_LENGTH(pulsar_button_map); i++) {
		if (ratbag_button_action_match(action, &pulsar_button_map[i].action)) {
			*mode = pulsar_button_map[i].mode;
			*param1 = pulsar_button_map[i].param1;
			return true;
		}
	}

	return false;
}

static void
pulsar_read_button(struct ratbag_button *button, const uint8_t *settings)
{
	unsigned int addr = PULSAR_ADDR_BUTTON_BASE + button->index * 4;
	uint8_t mode = settings[addr];
	uint8_t param1 = settings[addr + 1];
	const struct ratbag_button_action *action;

	action = pulsar_raw_to_action(mode, param1);
	if (action)
		ratbag_button_set_action(button, action);

	ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_NONE);
	ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_BUTTON);
	ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_SPECIAL);
}

static void
pulsar_read_led(struct ratbag_led *led, const uint8_t *settings)
{
	uint8_t effect = settings[PULSAR_ADDR_LED_EFFECT];
	uint8_t brightness = settings[PULSAR_ADDR_LED_BRIGHTNESS];
	uint8_t speed = settings[PULSAR_ADDR_LED_BREATHE_SPEED];
	uint8_t enabled = settings[PULSAR_ADDR_LED_ENABLED];

	if (!enabled) {
		led->mode = RATBAG_LED_OFF;
	} else if (effect == PULSAR_LED_BREATHE) {
		led->mode = RATBAG_LED_BREATHING;
	} else {
		led->mode = RATBAG_LED_ON;
	}

	led->brightness = brightness;

	/* Map speed 1-5 to ms: speed 1 = slowest = 5000ms, speed 5 = fastest = 1000ms */
	if (speed >= 1 && speed <= 5)
		led->ms = (6 - speed) * 1000;
	else
		led->ms = 3000;

	/* LED color comes from the active DPI mode's color */
	uint8_t active_dpi = settings[PULSAR_ADDR_ACTIVE_DPI_MODE];
	if (active_dpi < PULSAR_NUM_DPI_MODES) {
		unsigned int color_addr = PULSAR_ADDR_DPI_COLOR_BASE + active_dpi * 4;
		led->color.red = settings[color_addr];
		led->color.green = settings[color_addr + 1];
		led->color.blue = settings[color_addr + 2];
	}

	led->colordepth = RATBAG_LED_COLORDEPTH_RGB_888;

	ratbag_led_set_mode_capability(led, RATBAG_LED_OFF);
	ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
	ratbag_led_set_mode_capability(led, RATBAG_LED_BREATHING);
}

static void
pulsar_read_profile(struct ratbag_profile *profile, const uint8_t *settings)
{
	struct ratbag_resolution *resolution;
	struct ratbag_button *button;
	struct ratbag_led *led;
	unsigned int rate_hz;
	uint8_t active_dpi;
	unsigned int dpi_count;

	/* Polling rate */
	rate_hz = pulsar_polling_rate_to_hz(settings[PULSAR_ADDR_POLLING_RATE]);
	profile->hz = rate_hz;

	unsigned int report_rates[] = { 125, 250, 500, 1000 };
	ratbag_profile_set_report_rate_list(profile, report_rates,
					    ARRAY_LENGTH(report_rates));

	/* Debounce */
	profile->debounce = settings[PULSAR_ADDR_DEBOUNCE];
	static const unsigned int debounce_values[] = { 0, 2, 4, 6, 8, 10, 20, 30 };
	ratbag_profile_set_debounce_list(profile, debounce_values,
					 ARRAY_LENGTH(debounce_values));

	/* Angle snapping */
	profile->angle_snapping = settings[PULSAR_ADDR_ANGLE_SNAPPING];

	ratbag_profile_set_cap(profile, RATBAG_PROFILE_CAP_SET_DEFAULT);

	/* DPI modes */
	active_dpi = settings[PULSAR_ADDR_ACTIVE_DPI_MODE];
	dpi_count = settings[PULSAR_ADDR_DPI_MODE_COUNT];
	if (dpi_count > PULSAR_NUM_DPI_MODES)
		dpi_count = PULSAR_NUM_DPI_MODES;

	ratbag_profile_for_each_resolution(profile, resolution) {
		unsigned int dpi_addr = PULSAR_ADDR_DPI_BASE + resolution->index * 4;
		unsigned int color_addr = PULSAR_ADDR_DPI_COLOR_BASE + resolution->index * 4;
		unsigned int dpi;

		dpi = pulsar_decode_dpi(&settings[dpi_addr]);
		ratbag_resolution_set_resolution(resolution, dpi, dpi);
		ratbag_resolution_set_dpi_list_from_range(resolution,
							  PULSAR_DPI_MIN,
							  PULSAR_DPI_MAX);
		ratbag_resolution_set_cap(resolution, RATBAG_RESOLUTION_CAP_DISABLE);

		resolution->is_active = (resolution->index == active_dpi);
		resolution->is_disabled = (resolution->index >= dpi_count);

		/* Store per-resolution LED color in resolution's user data area.
		 * These colors are exposed via the LED interface when writing. */
		(void)color_addr;
	}

	/* Buttons */
	ratbag_profile_for_each_button(profile, button)
		pulsar_read_button(button, settings);

	/* LEDs */
	ratbag_profile_for_each_led(profile, led)
		pulsar_read_led(led, settings);
}

static int
pulsar_test_hidraw(struct ratbag_device *device)
{
	return ratbag_hidraw_has_report(device, PULSAR_REPORT_ID);
}

static int
pulsar_probe(struct ratbag_device *device)
{
	struct pulsar_data *drv_data;
	struct ratbag_profile *profile;
	int rc;

	rc = ratbag_find_hidraw(device, pulsar_test_hidraw);
	if (rc)
		return rc;

	drv_data = zalloc(sizeof(*drv_data));
	ratbag_set_drv_data(device, drv_data);

	/* Verify device is active */
	rc = pulsar_status_check(device);
	if (rc) {
		log_error(device->ratbag, "Pulsar: status check failed\n");
		goto err;
	}

	/* Get active profile */
	rc = pulsar_get_active_profile(device, &drv_data->active_profile);
	if (rc) {
		log_error(device->ratbag, "Pulsar: failed to get active profile\n");
		goto err;
	}

	/* Read all settings from device memory */
	rc = pulsar_read_settings(device, drv_data);
	if (rc)
		goto err;

	/* Initialize profile structure */
	rc = ratbag_device_init_profiles(device, PULSAR_NUM_PROFILES,
					 PULSAR_NUM_DPI_MODES,
					 PULSAR_NUM_BUTTONS,
					 PULSAR_NUM_LEDS);
	if (rc)
		goto err;

	/* Parse settings into profiles.
	 * Currently both profiles share the same settings region.
	 * Profile switching via ACTIVE_PROFILE_SET changes which
	 * profile the device uses, but the memory layout for per-profile
	 * settings is not yet fully understood. */
	ratbag_device_for_each_profile(device, profile) {
		pulsar_read_profile(profile, drv_data->settings);
		profile->is_active = (profile->index == drv_data->active_profile);
		profile->is_enabled = true;
	}

	return 0;

err:
	free(drv_data);
	ratbag_set_drv_data(device, NULL);
	ratbag_close_hidraw(device);
	return rc;
}

static void
pulsar_remove(struct ratbag_device *device)
{
	ratbag_close_hidraw(device);
	free(ratbag_get_drv_data(device));
}

static uint8_t
pulsar_setting_checksum(uint8_t value)
{
	return (0x55 - value) & 0xFF;
}

static uint8_t
pulsar_multi_checksum(const uint8_t *data, size_t len)
{
	unsigned int sum = 0;

	for (size_t i = 0; i < len; i++)
		sum += data[i];

	return (0x55 - sum) & 0xFF;
}

static int
pulsar_write_setting_byte(struct ratbag_device *device, uint32_t addr, uint8_t value)
{
	uint8_t data[2];

	data[0] = value;
	data[1] = pulsar_setting_checksum(value);

	return pulsar_mem_set(device, addr, data, 2);
}

static int
pulsar_write_dpi(struct ratbag_device *device, unsigned int mode_index,
		 unsigned int dpi)
{
	uint8_t data[4];
	uint32_t addr = PULSAR_ADDR_DPI_BASE + mode_index * 4;

	pulsar_encode_dpi(dpi, data);
	data[3] = pulsar_multi_checksum(data, 3);

	return pulsar_mem_set(device, addr, data, 4);
}

static int
pulsar_write_dpi_color(struct ratbag_device *device, unsigned int mode_index,
		       struct ratbag_color color)
{
	uint8_t data[4];
	uint32_t addr = PULSAR_ADDR_DPI_COLOR_BASE + mode_index * 4;

	data[0] = color.red;
	data[1] = color.green;
	data[2] = color.blue;
	data[3] = pulsar_multi_checksum(data, 3);

	return pulsar_mem_set(device, addr, data, 4);
}

static int
pulsar_write_button(struct ratbag_device *device, struct ratbag_button *button)
{
	uint8_t mode, param1;
	uint8_t data[4];
	uint32_t addr = PULSAR_ADDR_BUTTON_BASE + button->index * 4;

	if (!pulsar_action_to_raw(&button->action, &mode, &param1)) {
		log_error(device->ratbag,
			  "Pulsar: unsupported button action for button %d\n",
			  button->index);
		return -EINVAL;
	}

	data[0] = mode;
	data[1] = param1;
	data[2] = 0x00;
	data[3] = pulsar_multi_checksum(data, 3);

	return pulsar_mem_set(device, addr, data, 4);
}

static int
pulsar_write_led(struct ratbag_device *device, struct ratbag_led *led)
{
	int rc;
	uint8_t effect, enabled;

	switch (led->mode) {
	case RATBAG_LED_OFF:
		enabled = 0x00;
		effect = PULSAR_LED_STEADY;
		break;
	case RATBAG_LED_BREATHING:
		enabled = 0x01;
		effect = PULSAR_LED_BREATHE;
		break;
	case RATBAG_LED_ON:
	default:
		enabled = 0x01;
		effect = PULSAR_LED_STEADY;
		break;
	}

	rc = pulsar_write_setting_byte(device, PULSAR_ADDR_LED_ENABLED, enabled);
	if (rc)
		return rc;

	rc = pulsar_write_setting_byte(device, PULSAR_ADDR_LED_EFFECT, effect);
	if (rc)
		return rc;

	rc = pulsar_write_setting_byte(device, PULSAR_ADDR_LED_BRIGHTNESS,
				       led->brightness);
	if (rc)
		return rc;

	/* Convert ms back to speed: speed = 6 - (ms / 1000), clamped 1-5 */
	if (led->mode == RATBAG_LED_BREATHING) {
		unsigned int speed = 6 - (led->ms / 1000);
		if (speed < 1) speed = 1;
		if (speed > 5) speed = 5;

		rc = pulsar_write_setting_byte(device, PULSAR_ADDR_LED_BREATHE_SPEED,
					       speed);
		if (rc)
			return rc;
	}

	return 0;
}

static int
pulsar_commit(struct ratbag_device *device)
{
	struct ratbag_profile *profile;
	struct ratbag_resolution *resolution;
	struct ratbag_button *button;
	struct ratbag_led *led;
	int rc;

	/* Status check before writing */
	rc = pulsar_status_check(device);
	if (rc)
		return rc;

	ratbag_device_for_each_profile(device, profile) {
		if (!profile->dirty)
			continue;

		/* Polling rate */
		if (profile->rate_dirty) {
			uint8_t rate_val = pulsar_hz_to_polling_rate(profile->hz);
			rc = pulsar_write_setting_byte(device,
						       PULSAR_ADDR_POLLING_RATE,
						       rate_val);
			if (rc)
				return rc;
		}

		/* Debounce */
		if (profile->debounce_dirty) {
			rc = pulsar_write_setting_byte(device,
						       PULSAR_ADDR_DEBOUNCE,
						       profile->debounce);
			if (rc)
				return rc;
		}

		/* Angle snapping */
		if (profile->angle_snapping_dirty) {
			rc = pulsar_write_setting_byte(device,
						       PULSAR_ADDR_ANGLE_SNAPPING,
						       profile->angle_snapping ? 0x01 : 0x00);
			if (rc)
				return rc;
		}

		/* DPI resolutions */
		ratbag_profile_for_each_resolution(profile, resolution) {
			if (!resolution->dirty)
				continue;

			rc = pulsar_write_dpi(device, resolution->index,
					      resolution->dpi_x);
			if (rc)
				return rc;
		}

		/* Buttons */
		ratbag_profile_for_each_button(profile, button) {
			if (!button->dirty)
				continue;

			rc = pulsar_write_button(device, button);
			if (rc)
				return rc;
		}

		/* LEDs */
		ratbag_profile_for_each_led(profile, led) {
			if (!led->dirty)
				continue;

			rc = pulsar_write_led(device, led);
			if (rc)
				return rc;

			/* Write LED color to each DPI mode's color slot */
			for (unsigned int i = 0; i < PULSAR_NUM_DPI_MODES; i++) {
				rc = pulsar_write_dpi_color(device, i,
							    led->color);
				if (rc)
					return rc;
			}
		}
	}

	return 0;
}

static int
pulsar_set_active_profile(struct ratbag_device *device, unsigned int index)
{
	uint8_t pkt[PULSAR_PACKET_SIZE];
	uint8_t resp[PULSAR_PACKET_SIZE];
	uint8_t data[1] = { index };

	pulsar_build_packet(pkt, PULSAR_CMD_ACTIVE_PROFILE_SET, 0, 1, data, 1);
	return pulsar_transact(device, pkt, resp);
}

struct ratbag_driver pulsar_driver = {
	.name = "Pulsar Gen2",
	.id = "pulsar-gen2",
	.probe = pulsar_probe,
	.remove = pulsar_remove,
	.commit = pulsar_commit,
	.set_active_profile = pulsar_set_active_profile,
};
