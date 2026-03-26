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
#include <stddef.h>
#include <linux/input.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "libratbag-private.h"
#include "libratbag-hidraw.h"
#include "libratbag-util.h"

/* Device constants */
#define PULSAR_NUM_PROFILES		4
#define PULSAR_NUM_DPI_MODES		8
#define PULSAR_NUM_BUTTONS		6
#define PULSAR_NUM_BUTTONS_X2A		8
#define PULSAR_NUM_LEDS			(1 + PULSAR_NUM_DPI_MODES)

/* Packet format */
#define PULSAR_REPORT_ID		0x08
#define PULSAR_PACKET_SIZE		17
#define PULSAR_MEM_BATCH		10

/* Commands */
#define PULSAR_CMD_INFO			0x01
#define PULSAR_CMD_STATUS		0x03
#define PULSAR_CMD_MEM_SET		0x07
#define PULSAR_CMD_MEM_GET		0x08
#define PULSAR_CMD_DEVICE_EVENT		0x0A
#define PULSAR_CMD_ACTIVE_PROFILE_GET	0x0E
#define PULSAR_CMD_ACTIVE_PROFILE_SET	0x0F

#define PULSAR_EVENT_DPI		0x01
#define PULSAR_EVENT_PROFILE		0x04
#define PULSAR_EVENT_POWER		0x40

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
#define PULSAR_ADDR_LOD			0x0A
#define PULSAR_ADDR_DEBOUNCE		0xA9
#define PULSAR_ADDR_MOTION_SYNC		0xAB
#define PULSAR_ADDR_ANGLE_SNAPPING	0xAF
#define PULSAR_ADDR_RIPPLE_CONTROL	0xB1
#define PULSAR_ADDR_AUTOSLEEP		0xB7

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
#define PULSAR_RATE_4000		0x20
#define PULSAR_RATE_2000		0x10
#define PULSAR_RATE_1000		0x01
#define PULSAR_RATE_500			0x02
#define PULSAR_RATE_250			0x04
#define PULSAR_RATE_125			0x08

/* LED effect values */
#define PULSAR_LED_STEADY		0x01
#define PULSAR_LED_BREATHE		0x02

/* Combinations */
#define PULSAR_COMB_BASE_ADDR		0x0100
#define PULSAR_COMB_NUM_ACTIONS		10

/* Macros */
#define PULSAR_MACRO_BASE_ADDR		0x0300
#define PULSAR_NUM_MACROS		16
#define PULSAR_MACRO_NUM_ACTIONS	70

struct __attribute__((packed)) pulsar_combination_action
{
	uint8_t code;
	uint16_t value;  // little endian
};
static_assert(sizeof(struct pulsar_combination_action) == 3,
	"invalid combination action size");

struct __attribute__((packed)) pulsar_combination
{
	uint8_t count;
	struct pulsar_combination_action actions[PULSAR_COMB_NUM_ACTIONS];
	uint8_t checksum;
};
static_assert(sizeof(struct pulsar_combination) == 32,
	"invalid combination size");

struct __attribute__((packed)) pulsar_macro_action
{
	uint8_t code;
	uint16_t value;  // little endian
	uint16_t delay;  // big endian
};
static_assert(sizeof(struct pulsar_macro_action) == 5,
	"invalid macro action size");

struct __attribute__((packed)) pulsar_macro
{
	uint8_t name_length;
	uint16_t name[15];  // UTF-16LE
	uint8_t num_actions;
	struct pulsar_macro_action actions[PULSAR_MACRO_NUM_ACTIONS];
	// checksum is directly after the last action
	// --> only here if num_actions == PULSAR_MACRO_NUM_ACTIONS
	uint8_t checksum;
	uint8_t unused;
};
static_assert(sizeof(struct pulsar_macro) ==  384,
	"invalid macro size");

struct pulsar_memory
{
	uint8_t settings[PULSAR_SETTINGS_SIZE];
	struct pulsar_combination combination[PULSAR_NUM_BUTTONS_X2A];
	struct pulsar_macro macro[PULSAR_NUM_MACROS];
};

struct pulsar_data {
	struct pulsar_memory memory[PULSAR_NUM_PROFILES];
	uint8_t active_profile;
	size_t dpi_changed;
	size_t profile_changed;
};

struct pulsar_payload
{
	uint8_t header;
	uint8_t cmd;
	uint8_t data[14];
	uint8_t checksum;
};
static_assert(sizeof(struct pulsar_payload) == PULSAR_PACKET_SIZE, "payload size missmatch");

static uint8_t
pulsar_checksum(const uint8_t *buf, size_t len)
{
	unsigned int sum = 0;

	for (size_t i = 0; i < len; i++)
		sum += buf[i];

	return (0x55 - sum) & 0xFF;
}

static bool
pulsar_verify_setting(const uint8_t *settings, uint16_t addr, size_t len)
{
	uint8_t expected = pulsar_checksum(settings + addr, len);
	return expected == settings[addr + len];
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

static unsigned int
pulsar_polling_rate_to_hz(uint8_t val)
{
	switch (val) {
	case PULSAR_RATE_4000: return 4000;
	case PULSAR_RATE_2000: return 2000;
	case PULSAR_RATE_1000: return 1000;
	case PULSAR_RATE_500:  return 500;
	case PULSAR_RATE_250:  return 250;
	case PULSAR_RATE_125:  return 125;
	default: return 1000;
	}
}

static bool
pulsar_is_payload_ok(const struct pulsar_payload *payload)
{
	if (payload->header != PULSAR_REPORT_ID)
		return false;

	uint8_t checksum = pulsar_checksum((uint8_t *)payload, sizeof(*payload) - 1);
	return checksum == payload->checksum;
}

static void
pulsar_finalize_payload(struct pulsar_payload *payload, uint8_t cmd)
{
	payload->header = PULSAR_REPORT_ID;
	payload->cmd = cmd;
	payload->checksum = pulsar_checksum((uint8_t *)payload, sizeof(*payload) - 1);
}

static int
pulsar_send_command(struct ratbag_device *device, const struct pulsar_payload *payload)
{
	const int rc = ratbag_hidraw_output_report(device, (uint8_t*)payload,
	                                     sizeof(*payload));
	if (rc < 0)
		return rc;

	log_raw(device->ratbag, "%s: cmd=%02x\n", __func__, (int)payload->cmd);

	return 0;
}

static bool
pulsar_report_filter(uint8_t *buf, size_t len)
{
	if (len != sizeof(struct pulsar_payload))
		return false;

	struct pulsar_payload *payload = (struct pulsar_payload *)buf;

	return pulsar_is_payload_ok(payload);
}

static int
pulsar_read_response(struct ratbag_device *device, struct pulsar_payload *payload)
{
	int rc;
	struct pulsar_data *drv_data = device->drv_data;

	for (;;) {
		rc = ratbag_hidraw_read_input_report(device,
			(uint8_t *)payload, sizeof(*payload), pulsar_report_filter);

		if (rc < 0)
			return rc;

		if (payload->cmd != PULSAR_CMD_DEVICE_EVENT)
			break;

		if (payload->data[0] == PULSAR_EVENT_DPI)
			drv_data->dpi_changed++;
		else if (payload->data[0] == PULSAR_EVENT_PROFILE)
			drv_data->profile_changed++;

		log_debug(device->ratbag,
			  "discarding device event (type=0x%02x)\n",
			  payload->data[4]);
	}

	log_raw(device->ratbag, "%s: cmd=%02x\n", __func__, (int)payload->cmd);

	return rc;
}

static int
pulsar_transaction(struct ratbag_device *device, const struct pulsar_payload *request, struct pulsar_payload *response)
{
	int rc = pulsar_send_command(device, request);
	if (rc < 0)
		return rc;

	rc = pulsar_read_response(device, response);
	if (rc < 0)
		return rc;

	return 0;
}

static int
pulsar_read_status(struct ratbag_device *device)
{
	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	pulsar_finalize_payload(&request, PULSAR_CMD_STATUS);
	const int rc = pulsar_transaction(device, &request, &response);
	if (rc < 0)
		return rc;

	if (response.data[4] != 0x01)
		log_debug(device->ratbag, "Pulsar: device not active\n");

	return response.data[4];
}

static int
pulsar_read_info(struct ratbag_device *device, uint8_t device_info[4])
{
	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	request.data[3] = 8; // len

	for (size_t i = 0; i < 4; i++)
		request.data[i + 4] = rand();

	pulsar_finalize_payload(&request, PULSAR_CMD_INFO);
	const int rc = pulsar_transaction(device, &request, &response);
	if (rc < 0)
		return rc;

	memcpy(device_info, response.data + 8, 4);

	/*
	 * Verify challenge-response. Response layout from offset 6:
	 *   [0..3] encoded response   [4..7] device info (ID + conn type)
	 *
	 * resp[i] = challenge[i] * (i+1) + challenge[(i+1) % 4] + device_id[i]
	 *
	 * bytes 6..7 are zeroed for verification.
	 */
	response.data[10] = 0;
	response.data[11] = 0;

	for (size_t i = 0; i < 4; i++) {
		uint8_t expected = response.data[8 + i];
		uint8_t actual = response.data[4 + i] -
			(i + 1) * request.data[4 + i] -
			request.data[4 + (i + 1) % 4];

		if (actual != expected)
		{
			log_error(device->ratbag,
				"device info[%lu] mismatch: %02x != %02x\n",
				i, expected, actual);
			return -1;
		}
	}

	return 0;
}

static int
pulsar_read_active_profile(struct ratbag_device *device)
{
	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	pulsar_finalize_payload(&request, PULSAR_CMD_ACTIVE_PROFILE_GET);
	const int rc = pulsar_transaction(device, &request, &response);
	if (rc < 0)
		return rc;

	return response.data[4];
}

static int
pulsar_write_active_profile(struct ratbag_device *device, uint8_t index)
{
	if (index >= PULSAR_NUM_PROFILES)
		return -1;

	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	request.data[4] = (uint8_t)index;
	pulsar_finalize_payload(&request, PULSAR_CMD_ACTIVE_PROFILE_SET);
	const int rc = pulsar_transaction(device, &request, &response);
	if (rc < 0)
		return rc;

	return response.data[4];
}

static int
pulsar_read_memory(struct ratbag_device *device, uint16_t addr, uint8_t *buf, size_t len)
{
	if (len > PULSAR_MEM_BATCH)
		return -1;

	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	request.data[1] = (uint8_t) (addr >> 8);
	request.data[2] = (uint8_t) (addr & 0xff);
	request.data[3] = (uint8_t) len;
	pulsar_finalize_payload(&request, PULSAR_CMD_MEM_GET);

	const int rc = pulsar_transaction(device, &request, &response);
	if (rc < 0)
		return rc;

	memcpy(buf, response.data + 4, len);
	return 0;
}

static int
pulsar_read_memory_area(struct ratbag_device *device, uint16_t addr,
	uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		uint16_t batch_addr = addr + done;
		size_t batch_len = min(len - done, PULSAR_MEM_BATCH);

		int rc = pulsar_read_memory(device, batch_addr, buf + done, batch_len);
		if (rc < 0)
			return rc;

		done += batch_len;
	}

	return 0;
}

static int
pulsar_write_memory(struct ratbag_device *device, uint16_t addr,
		    const uint8_t *data, size_t len)
{
	if (len > PULSAR_MEM_BATCH)
		return -1;

	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	request.data[1] = (uint8_t)(addr >> 8);
	request.data[2] = (uint8_t)(addr & 0xff);
	request.data[3] = (uint8_t)len;
	memcpy(request.data + 4, data, len);
	pulsar_finalize_payload(&request, PULSAR_CMD_MEM_SET);

	return pulsar_transaction(device, &request, &response);
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
pulsar_write_setting_byte(struct ratbag_device *device, uint16_t addr,
			  uint8_t value)
{
	uint8_t data[2];

	data[0] = value;
	data[1] = pulsar_setting_checksum(value);

	return pulsar_write_memory(device, addr, data, 2);
}

static void
pulsar_encode_dpi(unsigned int dpi, uint8_t *bytes)
{
	unsigned int factor12800 = 0;
	unsigned int remainder;
	uint8_t factor50;

	if (dpi > 25600)
		factor12800 = 2;
	else if (dpi > 12800)
		factor12800 = 1;

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

static int
pulsar_write_dpi(struct ratbag_device *device, unsigned int mode_index,
		 unsigned int dpi)
{
	uint8_t data[4];
	uint16_t addr = PULSAR_ADDR_DPI_BASE + mode_index * 4;

	pulsar_encode_dpi(dpi, data);
	data[3] = pulsar_multi_checksum(data, 3);

	return pulsar_write_memory(device, addr, data, 4);
}

static int
pulsar_write_dpi_color(struct ratbag_device *device, unsigned int mode_index,
		       struct ratbag_color color)
{
	uint8_t data[4];
	uint16_t addr = PULSAR_ADDR_DPI_COLOR_BASE + mode_index * 4;

	log_debug(device->ratbag,
		"pulsar_write_dpi_color index=%u color=%02x%02x%02x\n",
		mode_index, color.red, color.green, color.blue);

	data[0] = color.red;
	data[1] = color.green;
	data[2] = color.blue;
	data[3] = pulsar_multi_checksum(data, 3);

	return pulsar_write_memory(device, addr, data, 4);
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

	if (led->mode == RATBAG_LED_BREATHING) {
		unsigned int speed = 6 - (led->ms / 1000);
		if (speed < 1) speed = 1;
		if (speed > 5) speed = 5;

		rc = pulsar_write_setting_byte(device,
					       PULSAR_ADDR_LED_BREATHE_SPEED,
					       speed);
		if (rc)
			return rc;
	}

	return 0;
}

static uint8_t
pulsar_hz_to_polling_rate(unsigned int hz)
{
	switch (hz) {
	case 4000: return PULSAR_RATE_4000;
	case 2000: return PULSAR_RATE_2000;
	case 1000: return PULSAR_RATE_1000;
	case 500:  return PULSAR_RATE_500;
	case 250:  return PULSAR_RATE_250;
	case 125:  return PULSAR_RATE_125;
	default:   return PULSAR_RATE_1000;
	}
}

static int
pulsar_read_combination (struct ratbag_device *device,
	struct pulsar_combination *combination, size_t index, size_t profile)
{
	int ret;
	const uint16_t addr = PULSAR_COMB_BASE_ADDR +
		index * sizeof(*combination);

	log_debug(device->ratbag, "%s: profile=%lu index=%lu\n", __func__,
		profile, index);

	ret = pulsar_read_memory_area(device, addr, (uint8_t *)combination,
		sizeof(*combination));
	if (ret < 0)
		return ret;

	// empty combination
	if (combination->count == 0 || combination->count == 0xFF) {
		combination->count = 0;
		return 0;
	}

	// verify checksum
	const uint8_t expected = pulsar_checksum((uint8_t *)combination,
		sizeof(*combination) - 1);
	if (expected != combination->checksum) {
		log_error(device->ratbag,
			"invalid combination checksum %02x != %02x profile=%lu index=%lu\n",
			expected, combination->checksum, profile, index);
		combination->count = 0;
	}

	return 0;
}

static int
pulsar_read_macro (struct ratbag_device *device,
	struct pulsar_macro *macro, size_t index, size_t profile)
{
	log_debug(device->ratbag, "%s: profile=%lu index=%lu\n", __func__,
		profile, index);

	int ret;
	const uint16_t addr = PULSAR_MACRO_BASE_ADDR + index * sizeof(*macro);
	ptrdiff_t action_offset = sizeof(macro->name_length) +
		sizeof(macro->name) + sizeof(macro -> num_actions);

	// name and action count
	ret = pulsar_read_memory_area(device, addr, (uint8_t *)macro,
		action_offset);
	if (ret < 0)
		return ret;

	// empty macro
	if (macro->num_actions == 0 || macro->num_actions == 0xFF ||
		macro->name_length == 0xFF) {
		macro-> num_actions = 0;
		return 0;
	}

	// read actions and checksum
	size_t action_size = sizeof(*macro->actions) *
		min(macro->num_actions, PULSAR_MACRO_NUM_ACTIONS) + 1;

	ret = pulsar_read_memory_area(device, addr + action_offset,
		(uint8_t *) macro + action_offset, action_size);
	if (ret < 0)
		return ret;

	// verify checksum
	uint8_t expected = pulsar_checksum((uint8_t *)macro,
		action_offset + action_size - 1);
	uint8_t checksum = *(((uint8_t *) macro) + action_offset + action_size - 1);
	if (expected != checksum) {
		log_error(device->ratbag,
			"invalid combination checksum %02x != %02x profile=%lu index=%lu\n",
			expected, checksum, profile, index);
		macro->num_actions = 0;
	}

	return 0;
}

static int
pulsar_read_profile_settings(struct ratbag_profile *profile)
{
	log_debug(profile->device->ratbag, "%s: index=%u\n", __func__,
		profile->index);

	int active_profile = pulsar_read_active_profile(profile->device);
	if (active_profile < 0)
		return active_profile;

	struct pulsar_data *drv_data = profile->drv_data;
	assert(drv_data != NULL);
	int ret = 0;
	size_t profile_changed_before = drv_data->profile_changed;

	/* switch to target profile if needed */
	if ((unsigned)active_profile != profile->index) {
		ret = pulsar_write_active_profile(profile->device, profile->index);
		if (ret < 0)
			return ret;
	}

	/* read settings */
	uint8_t *settings = drv_data->memory[profile->index].settings;
	ret = pulsar_read_memory_area(profile->device, 0, settings, PULSAR_SETTINGS_SIZE);
	if (ret < 0)
		goto out;

	/* read combinations */
	for (size_t i = 0; i < PULSAR_NUM_BUTTONS_X2A; i++) {
		ret = pulsar_read_combination(profile->device,
			drv_data->memory->combination + i, i, profile->index);
		if (ret < 0)
			goto out;
	}

	/* read macros */
	for (size_t i = 0; i < PULSAR_NUM_MACROS; i++) {
		ret = pulsar_read_macro(profile->device,
			drv_data->memory->macro + i, i, profile->index);
		if (ret < 0)
			goto out;
	}

	/* restore active profile */
	if ((unsigned) active_profile != profile->index)
	{
		ret = pulsar_write_active_profile(profile->device, active_profile);
		if (ret < 0) {
			log_error(profile->device->ratbag,
				"failed to restore active profile (%d)", ret);
			// parse settings even if profile restore failed
		} else {
			log_debug(profile->device->ratbag,
				"active profile restored");
		}
	}

	/* check if profile was changed during read */
	if (drv_data->profile_changed != profile_changed_before) {
		log_error(profile->device->ratbag,
			  "profile changed during read, aborting\n");
		ret = -EAGAIN;
		goto out;
	}

	/* ----- verify setting checksums ----- */
	static const struct { uint16_t addr; size_t len; const char *name; } setting_checksums[] = {
		{ PULSAR_ADDR_POLLING_RATE,    1, "polling rate" },
		{ PULSAR_ADDR_DPI_MODE_COUNT,  1, "dpi mode count" },
		{ PULSAR_ADDR_ACTIVE_DPI_MODE, 1, "active dpi mode" },
		{ PULSAR_ADDR_LOD,             1, "lod" },
		{ PULSAR_ADDR_DEBOUNCE,        1, "debounce" },
		{ PULSAR_ADDR_MOTION_SYNC,     1, "motion sync" },
		{ PULSAR_ADDR_ANGLE_SNAPPING,  1, "angle snapping" },
		{ PULSAR_ADDR_RIPPLE_CONTROL,  1, "ripple control" },
		{ PULSAR_ADDR_AUTOSLEEP,       1, "autosleep" },
		{ PULSAR_ADDR_LED_EFFECT,      1, "led effect" },
		{ PULSAR_ADDR_LED_BRIGHTNESS,  1, "led brightness" },
		{ PULSAR_ADDR_LED_BREATHE_SPEED, 1, "led breathe speed" },
		{ PULSAR_ADDR_LED_ENABLED,     1, "led enabled" },
	};

	for (size_t i = 0; i < ARRAY_LENGTH(setting_checksums); i++) {
		if (!pulsar_verify_setting(settings, setting_checksums[i].addr,
					   setting_checksums[i].len)) {
			log_error(profile->device->ratbag,
				  "invalid checksum for %s at 0x%04x\n",
				  setting_checksums[i].name,
				  setting_checksums[i].addr);
			ret = -EINVAL;
			goto out;
		}
	}

	/* verify DPI value checksums (3 bytes + checksum) */
	uint8_t dpi_count = settings[PULSAR_ADDR_DPI_MODE_COUNT];
	for (unsigned int i = 0; i < PULSAR_NUM_DPI_MODES; i++) {
		uint16_t dpi_addr = PULSAR_ADDR_DPI_BASE + i * 4;
		if (!pulsar_verify_setting(settings, dpi_addr, 3)) {
			log_error(profile->device->ratbag,
				  "invalid checksum for dpi mode %u at 0x%04x\n",
				  i, dpi_addr);
			ret = -EINVAL;
			goto out;
		}

		uint16_t color_addr = PULSAR_ADDR_DPI_COLOR_BASE + i * 4;
		if (!pulsar_verify_setting(settings, color_addr, 3)) {
			log_error(profile->device->ratbag,
				  "invalid checksum for dpi color %u at 0x%04x\n",
				  i, color_addr);
			ret = -EINVAL;
			goto out;
		}
	}

	/* ----- parse settings ----- */
	/* polling rate */
	profile->hz = pulsar_polling_rate_to_hz(settings[PULSAR_ADDR_POLLING_RATE]);
	log_debug(profile->device->ratbag, "  polling rate: %u Hz\n", profile->hz);

	/* dpi mode */
	uint8_t active_dpi = settings[PULSAR_ADDR_ACTIVE_DPI_MODE];

	struct ratbag_resolution *resolution;
	ratbag_profile_for_each_resolution(profile, resolution) {
		uint16_t dpi_addr = PULSAR_ADDR_DPI_BASE + resolution->index * 4;
		unsigned int dpi = pulsar_decode_dpi(settings + dpi_addr);
		ratbag_resolution_set_resolution(resolution, dpi, dpi);

		resolution->is_active = (resolution->index == active_dpi);
		resolution->is_disabled = (resolution->index >= dpi_count);
		log_debug(profile->device->ratbag,
			  "  dpi mode %u: %u dpi%s%s\n",
			  resolution->index, dpi,
			  resolution->is_active ? " (active)" : "",
			  resolution->is_disabled ? " (disabled)" : "");
	}

	/* debounce */
	profile->debounce = settings[PULSAR_ADDR_DEBOUNCE];
	log_debug(profile->device->ratbag, "  debounce: %d ms\n",
		  profile->debounce);

	/* angle snapping */
	profile->angle_snapping = settings[PULSAR_ADDR_ANGLE_SNAPPING];
	log_debug(profile->device->ratbag, "  angle snapping: %s\n",
		  profile->angle_snapping ? "on" : "off");

	/* LED effect (LED 0) */
	struct ratbag_led *led;
	ratbag_profile_for_each_led(profile, led) {
		if (led->index == 0) {
			uint8_t enabled = settings[PULSAR_ADDR_LED_ENABLED];
			uint8_t effect = settings[PULSAR_ADDR_LED_EFFECT];
			uint8_t brightness = settings[PULSAR_ADDR_LED_BRIGHTNESS];
			uint8_t speed = settings[PULSAR_ADDR_LED_BREATHE_SPEED];

			if (!enabled)
				led->mode = RATBAG_LED_OFF;
			else if (effect == PULSAR_LED_BREATHE)
				led->mode = RATBAG_LED_BREATHING;
			else
				led->mode = RATBAG_LED_ON;

			led->brightness = brightness;

			if (speed >= 1 && speed <= 5)
				led->ms = (6 - speed) * 1000;
			else
				led->ms = 3000;

			/* color from active DPI mode */
			if (active_dpi < PULSAR_NUM_DPI_MODES) {
				unsigned int ca = PULSAR_ADDR_DPI_COLOR_BASE + active_dpi * 4;
				led->color.red = settings[ca];
				led->color.green = settings[ca + 1];
				led->color.blue = settings[ca + 2];
			}

			log_debug(profile->device->ratbag,
				  "  led effect: mode=%d brightness=%u ms=%u\n",
				  led->mode, led->brightness, led->ms);
		} else {
			/* LEDs 1-8: per-DPI-mode color */
			unsigned int ci = led->index - 1;
			unsigned int ca = PULSAR_ADDR_DPI_COLOR_BASE + ci * 4;
			led->color.red = settings[ca];
			led->color.green = settings[ca + 1];
			led->color.blue = settings[ca + 2];

			if (led->color.red || led->color.green || led->color.blue)
				led->mode = RATBAG_LED_ON;
			else
				led->mode = RATBAG_LED_OFF;

			log_debug(profile->device->ratbag,
				  "  dpi %u color: %02x%02x%02x\n",
				  ci, led->color.red, led->color.green,
				  led->color.blue);
		}
	}

	/* TODO: expose these settings to piper (no libratbag API yet)
	 *   - LOD:            settings[PULSAR_ADDR_LOD] (1-2 mm)
	 *   - motion sync:    settings[PULSAR_ADDR_MOTION_SYNC] (0=off, 1=on)
	 *   - ripple control: settings[PULSAR_ADDR_RIPPLE_CONTROL] (0=off, 1=on)
	 *   - auto-sleep:     settings[PULSAR_ADDR_AUTOSLEEP] (value * 10 = seconds, 10-600s)
	 */

	/* TODO:
		- buttons
		- combinations
		- macros
	 */

out:
	return ret;
}

static int
pulsar_test_hidraw(struct ratbag_device *device)
{
	return ratbag_hidraw_has_report(device, PULSAR_REPORT_ID);
}

static int
pulsar_read_profile(struct ratbag_profile *profile)
{
	struct ratbag_device *device = profile->device;

	int rc = pulsar_read_status(device);
	if (rc < 0)
		return rc;
	if (rc == 0) {
		log_error(device->ratbag,
			  "%s: device not responding (mouse asleep?)\n",
			  __func__);
		return -EAGAIN;
	}

	return pulsar_read_profile_settings(profile);
}

static bool
is_x2a(const uint8_t device_info[4])
{
	if (device_info[0] != 6)
		return false;

	return device_info[1] == 0x08 || device_info[1] == 0x09;
}

static bool
supports_4k_polling(const uint8_t device_info[4])
{
	return device_info[2] == 0x01;
}


static int
pulsar_probe(struct ratbag_device *device)
{
	struct ratbag_profile *profile;
	uint8_t device_info[4];
	unsigned int *report_rates;

	int rc = ratbag_find_hidraw(device, pulsar_test_hidraw);
	if (rc)
		return rc;

	struct pulsar_data *drv_data = zalloc(sizeof(*drv_data));
	ratbag_set_drv_data(device, drv_data);

	rc = pulsar_read_info(device, device_info);
	if (rc < 0)
		return rc;

	log_debug(device->ratbag, "%s: device_info=%02x %02x %02x %02x\n",
		__func__, device_info[0], device_info[1], device_info[2],
		device_info[3]);

	/* initialize profiles with structural data only */
	rc = ratbag_device_init_profiles(device, PULSAR_NUM_PROFILES,
					 PULSAR_NUM_DPI_MODES,
					 is_x2a(device_info) ? PULSAR_NUM_BUTTONS_X2A : PULSAR_NUM_BUTTONS,
					 PULSAR_NUM_LEDS);
	if (rc < 0)
		goto err;

	const size_t num_rates = supports_4k_polling(device_info) ? 6 : 4;
	report_rates = calloc(num_rates, sizeof(*report_rates));
	unsigned rate = 125;
	for (size_t i = 0; i < num_rates; i++) {
		report_rates[i] = rate;
		rate <<= 1;
	}

	ratbag_device_for_each_profile(device, profile) {
		struct ratbag_resolution *resolution;

		profile->drv_data = drv_data;
		profile->is_enabled = true;

		/* set structural/capability data for all profiles */
		ratbag_profile_set_report_rate_list(profile, report_rates,
						    num_rates);

		unsigned int debounce_values[31];
		for (unsigned int i = 0; i <= 30; i++)
			debounce_values[i] = i;
		ratbag_profile_set_debounce_list(profile, debounce_values,
						 ARRAY_LENGTH(debounce_values));

		ratbag_profile_for_each_resolution(profile, resolution) {
			ratbag_resolution_set_dpi_list_from_range(resolution,
								  PULSAR_DPI_MIN,
								  PULSAR_DPI_MAX);
			ratbag_resolution_set_cap(resolution,
						  RATBAG_RESOLUTION_CAP_DISABLE);
		}

		struct ratbag_led *led;
		ratbag_profile_for_each_led(profile, led) {
			if (led->index == 0) {
				/* LED 0: global effect (on/off/breathing) */
				ratbag_led_set_mode_capability(led, RATBAG_LED_OFF);
				ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
				ratbag_led_set_mode_capability(led, RATBAG_LED_BREATHING);
			} else {
				/* LEDs 1-8: per-DPI-mode color */
				ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
			}
			led->colordepth = RATBAG_LED_COLORDEPTH_RGB_888;
		}
	}

	/* try to read active profile if mouse is awake */
	rc = pulsar_read_status(device);
	if (rc < 0)
		goto err;

	if (rc != 0) {
		int active_profile = pulsar_read_active_profile(device);
		if (active_profile < 0)
			goto err;
		drv_data->active_profile = active_profile;

		ratbag_device_for_each_profile(device, profile) {
			profile->is_active = (profile->index == (unsigned)active_profile);
			if (profile->is_active) {
				rc = pulsar_read_profile_settings(profile);
				if (rc < 0)
					goto err;
				profile->loaded = true;
			}
		}
	} else {
		log_info(device->ratbag,
			 "Pulsar device not responding, deferring profile reads\n");
		/* mark first profile as active as placeholder */
		ratbag_device_for_each_profile(device, profile) {
			profile->is_active = (profile->index == 0);
		}
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

static int
pulsar_commit(struct ratbag_device *device)
{
	struct ratbag_profile *profile;
	struct ratbag_resolution *resolution;
	struct ratbag_button *button;
	struct ratbag_led *led;

	ratbag_device_for_each_profile(device, profile) {
		if (!profile->dirty)
			continue;

		log_info(device->ratbag,
			 "commit: profile %d dirty (active=%d, enabled=%d, hz=%u)\n",
			 profile->index, profile->is_active, profile->is_enabled,
			 profile->hz);

		if (profile->rate_dirty)
			log_info(device->ratbag,
				 "  report rate: %u Hz\n", profile->hz);
		if (profile->angle_snapping_dirty)
			log_info(device->ratbag,
				 "  angle snapping: %d\n", profile->angle_snapping);
		if (profile->debounce_dirty)
			log_info(device->ratbag,
				 "  debounce: %d ms\n", profile->debounce);

		ratbag_profile_for_each_resolution(profile, resolution) {
			if (!resolution->dirty)
				continue;
			log_info(device->ratbag,
				 "  resolution %d dirty: dpi=%ux%u active=%d disabled=%d\n",
				 resolution->index, resolution->dpi_x,
				 resolution->dpi_y, resolution->is_active,
				 resolution->is_disabled);
		}

		ratbag_profile_for_each_button(profile, button) {
			if (!button->dirty)
				continue;
			log_info(device->ratbag,
				 "  button %d dirty: action type=%d\n",
				 button->index, button->action.type);
		}

		ratbag_profile_for_each_led(profile, led) {
			if (!led->dirty)
				continue;
			log_info(device->ratbag,
				 "  led %d dirty: mode=%d color=%02x%02x%02x brightness=%u ms=%u\n",
				 led->index, led->mode,
				 led->color.red, led->color.green, led->color.blue,
				 led->brightness, led->ms);
		}
	}

	/* The device only supports a contiguous range of DPI modes (0 to
	 * dpi_count-1). Re-enable any disabled modes below the highest
	 * enabled one so there are no gaps. */
	ratbag_device_for_each_profile(device, profile) {
		unsigned int highest_enabled = 0;

		ratbag_profile_for_each_resolution(profile, resolution) {
			if (!resolution->is_disabled)
				highest_enabled = resolution->index;
		}

		ratbag_profile_for_each_resolution(profile, resolution) {
			if (resolution->index <= highest_enabled &&
			    resolution->is_disabled) {
				resolution->is_disabled = false;
				log_info(device->ratbag,
					 "re-enabling dpi mode %u to fill gap\n",
					 resolution->index);
			}
		}
	}

	/* status check before writing */
	int rc = pulsar_read_status(device);
	if (rc < 0)
		return rc;
	if (rc == 0) {
		log_error(device->ratbag,
			  "commit: device not responding (mouse asleep?)\n");
		return -EAGAIN;
	}

	int original_profile = pulsar_read_active_profile(device);
	if (original_profile < 0)
		return original_profile;

	int current_profile = original_profile;

	ratbag_device_for_each_profile(device, profile) {
		if (!profile->dirty)
			continue;

		/* switch to target profile if needed */
		if (profile->index != (unsigned)current_profile) {
			rc = pulsar_write_active_profile(device, profile->index);
			if (rc < 0)
				goto restore;
			current_profile = profile->index;
		}

		/* polling rate */
		if (profile->rate_dirty) {
			uint8_t rate_val = pulsar_hz_to_polling_rate(profile->hz);
			rc = pulsar_write_setting_byte(device,
						       PULSAR_ADDR_POLLING_RATE,
						       rate_val);
			if (rc)
				goto restore;
		}

		/* debounce */
		if (profile->debounce_dirty) {
			rc = pulsar_write_setting_byte(device,
						       PULSAR_ADDR_DEBOUNCE,
						       profile->debounce);
			if (rc)
				goto restore;
		}

		/* angle snapping */
		if (profile->angle_snapping_dirty) {
			rc = pulsar_write_setting_byte(device,
						       PULSAR_ADDR_ANGLE_SNAPPING,
						       profile->angle_snapping ? 0x01 : 0x00);
			if (rc)
				goto restore;
		}

		/* DPI resolutions */
		unsigned int dpi_count = 0;
		ratbag_profile_for_each_resolution(profile, resolution) {
			if (!resolution->is_disabled)
				dpi_count = resolution->index + 1;
		}

		ratbag_profile_for_each_resolution(profile, resolution) {
			if (!resolution->dirty)
				continue;

			rc = pulsar_write_dpi(device, resolution->index,
					      resolution->dpi_x);
			if (rc)
				goto restore;
		}

		/* update dpi mode count */
		rc = pulsar_write_setting_byte(device,
					       PULSAR_ADDR_DPI_MODE_COUNT,
					       dpi_count);
		if (rc)
			goto restore;

		/* LEDs */
		ratbag_profile_for_each_led(profile, led) {
			if (!led->dirty)
				continue;

			if (led->index == 0) {
				/* LED 0: global effect settings */
				rc = pulsar_write_led(device, led);
			} else {
				/* LEDs 1-8: per-DPI-mode color */
				rc = pulsar_write_dpi_color(device,
							    led->index - 1,
							    led->color);
			}
			if (rc)
				goto restore;
		}
	}

	rc = 0;

restore:
	/* restore original active profile */
	if (current_profile != original_profile) {
		int rc2 = pulsar_write_active_profile(device, original_profile);
		if (rc2 < 0)
			log_error(device->ratbag,
				  "commit: failed to restore active profile\n");
	}

	return rc;
}

static int
pulsar_set_active_profile(struct ratbag_device *device, unsigned int index)
{
	if (index >= PULSAR_NUM_PROFILES)
		return -EINVAL;

	int rc = pulsar_write_active_profile(device, index);
	if (rc < 0)
		return rc;

	struct pulsar_data *drv_data = ratbag_get_drv_data(device);
	drv_data->active_profile = index;

	return 0;
}

struct ratbag_driver pulsar_driver = {
	.name = "Pulsar",
	.id = "pulsar",
	.probe = pulsar_probe,
	.read_profile = pulsar_read_profile,
	.remove = pulsar_remove,
	.commit = pulsar_commit,
	.set_active_profile = pulsar_set_active_profile,
};
