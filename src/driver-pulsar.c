/*
 * Copyright © 2026 Nikolas Koesling
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
#include <endian.h>
#include <errno.h>
#include <stddef.h>

#include "libratbag-data.h"
#include "libratbag-private.h"

/* Driver settings */
#define DRV_INIT_RETRY		1
#define DRV_RETRY_DELAY_S	1
#define DRV_VERIFY_PROFILE	1

/* Device constants */
#define PULSAR_NUM_PROFILES	4
#define PULSAR_NUM_DPI_MODES	8
#define PULSAR_NUM_BUTTONS	6
#define PULSAR_NUM_BUTTONS_X2A	8
#define PULSAR_NUM_LEDS		(1 + PULSAR_NUM_DPI_MODES)
#define PULSAR_MIN_DEBOUNCE	0
#define PULSAR_MAX_DEBOUNCE	30
#define PULSAR_NUM_DEBOUNCE	(PULSAR_MAX_DEBOUNCE - PULSAR_MIN_DEBOUNCE + 1)
#define PULSAR_CHECKSUM_MAGIC	0x55

/* Packet format */
#define PULSAR_REPORT_ID		0x08
#define PULSAR_PACKET_SIZE		17
#define PULSAR_MEM_BATCH		10
#define PULSAR_INFO_SIZE		4

/* Commands */
#define PULSAR_CMD_INFO			0x01
#define PULSAR_CMD_STATUS		0x03
#define PULSAR_CMD_MEM_SET		0x07
#define PULSAR_CMD_MEM_GET		0x08
#define PULSAR_CMD_RESTORE		0x09
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
#define PULSAR_DPI_STEP			50

/* Button modes */
#define PULSAR_BTN_MODE_DISABLED	0x00
#define PULSAR_BTN_MODE_MOUSE		0x01
#define PULSAR_BTN_MODE_DPI		0x02
#define PULSAR_BTN_MODE_COMBO		0x05
#define PULSAR_BTN_MODE_MACRO		0x06
#define PULSAR_BTN_MODE_PROFILE		0x09
#define PULSAR_BTN_MODE_DPI_LOCK	0x0A

/* Combination action codes */
#define PULSAR_ACTION_MOD_PRESS		0x80
#define PULSAR_ACTION_KEY_PRESS		0x81
#define PULSAR_ACTION_CONSUMER_PRESS	0x82
#define PULSAR_ACTION_MOD_RELEASE	0x40
#define PULSAR_ACTION_KEY_RELEASE	0x41
#define PULSAR_ACTION_CONSUMER_RELEASE	0x42

/* Macro action codes (same as combination, plus mouse) */
#define PULSAR_ACTION_MOUSE_PRESS	0x84
#define PULSAR_ACTION_MOUSE_RELEASE	0x44

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

/* Breathe speed mapping: speed 1-5, linear ms = -1500*speed + 10000 */
#define MIN_BREATH_SPEED		1
#define MAX_BREATH_SPEED		5
#define BREATHE_SPEED_SLOPE		1500
#define BREATHE_SPEED_OFFSET		10000
#define MIN_BREATHE_PERIOD_MS		(-BREATHE_SPEED_SLOPE * MAX_BREATH_SPEED + BREATHE_SPEED_OFFSET)
#define MAX_BREATHE_PERIOD_MS		(-BREATHE_SPEED_SLOPE * MIN_BREATH_SPEED + BREATHE_SPEED_OFFSET)

/* Combinations */
#define PULSAR_COMB_BASE_ADDR		0x0100
#define PULSAR_COMB_NUM_ACTIONS		10

/* Macros */
#define PULSAR_MACRO_BASE_ADDR		0x0300
#define PULSAR_NUM_MACROS		16
#define PULSAR_MACRO_NUM_ACTIONS	70

static const struct {
	uint8_t bit;
	unsigned int key;
} pulsar_mod_map[] = {
	{ 0x01, KEY_LEFTCTRL },
	{ 0x02, KEY_LEFTSHIFT },
	{ 0x04, KEY_LEFTALT },
	{ 0x08, KEY_LEFTMETA },
};

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

/* Mouse button bitmask <-> BTN_* keycode mapping */
static const struct {
	uint8_t bit;
	unsigned int keycode;
} pulsar_mouse_map[] = {
	{ PULSAR_MOUSE_LEFT,    BTN_LEFT },
	{ PULSAR_MOUSE_RIGHT,   BTN_RIGHT },
	{ PULSAR_MOUSE_MIDDLE,  BTN_MIDDLE },
	{ PULSAR_MOUSE_BACK,    BTN_SIDE },
	{ PULSAR_MOUSE_FORWARD, BTN_EXTRA },
};

static uint8_t
pulsar_mouse_btn_from_keycode(unsigned int keycode)
{
	for (size_t i = 0; i < ARRAY_LENGTH(pulsar_mouse_map); i++) {
		if (pulsar_mouse_map[i].keycode == keycode)
			return pulsar_mouse_map[i].bit;
	}
	return 0;
}

/* ----- device calculations ----- */
static uint8_t
pulsar_checksum(const uint8_t *buf, size_t len)
{
	unsigned int sum = 0;

	for (size_t i = 0; i < len; i++)
		sum += buf[i];

	return (PULSAR_CHECKSUM_MAGIC - sum) & 0xFF;
}

static unsigned int
pulsar_decode_dpi(const uint8_t *bytes)
{
	const unsigned int factor50 = bytes[0];
	const uint8_t factor12800_byte = bytes[2];
	unsigned int factor12800 = 0;

	if (factor12800_byte == 0x44)
		factor12800 = 1;
	else if (factor12800_byte == 0x88)
		factor12800 = 2;

	return (factor50 + 1) * 50 + factor12800 * 12800;
}

static int
pulsar_encode_dpi(unsigned int dpi, uint8_t *bytes)
{
	if (dpi > PULSAR_DPI_MAX)
		return -EINVAL;

	unsigned int factor12800 = 0;

	if (dpi > 25600)
		factor12800 = 2;
	else if (dpi > 12800)
		factor12800 = 1;

	const unsigned int remainder = dpi - factor12800 * 12800;
	const uint8_t factor50 = (remainder / 50) - 1;

	bytes[0] = factor50;
	bytes[1] = factor50;

	switch (factor12800) {
	case 0: bytes[2] = 0x00; break;
	case 1: bytes[2] = 0x44; break;
	case 2: bytes[2] = 0x88; break;
	}

	return 0;
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
	default:   return 0;
	}
}

static bool
pulsar_verify_setting(const uint8_t *settings, uint16_t addr, size_t len)
{
	const uint8_t expected = pulsar_checksum(settings + addr, len);
	return expected == settings[addr + len];
}

/* ----- payload helper ----- */
static bool
pulsar_is_payload_ok(const struct pulsar_payload *payload)
{
	if (payload->header != PULSAR_REPORT_ID)
		return false;

	const uint8_t checksum =
		pulsar_checksum((uint8_t *)payload, sizeof(*payload) - 1);
	return checksum == payload->checksum;
}

static void
pulsar_finalize_payload(struct pulsar_payload *payload, uint8_t cmd)
{
	payload->header = PULSAR_REPORT_ID;
	payload->cmd = cmd;
	payload->checksum =
		pulsar_checksum((uint8_t *)payload, sizeof(*payload) - 1);
}

/* ----- device i/o ----- */

static int
pulsar_send_command(struct ratbag_device *device,
	const struct pulsar_payload *payload)
{
	const int rc = ratbag_hidraw_output_report(
		device, (uint8_t*)payload, sizeof(*payload));
	if (rc < 0)
		return rc;

	log_raw(device->ratbag, "%s: cmd=%02x\n", __func__, (int)payload->cmd);
	log_buf_raw(device->ratbag, "pulsar_send_command data: ",
		(uint8_t *)payload, sizeof(*payload));

	return 0;
}

static void
pulsar_count_event(const struct ratbag_device *device,
		   const struct pulsar_payload *payload)
{
	struct pulsar_data *drv_data = device->drv_data;

	switch (payload->data[4]) {
	case PULSAR_EVENT_DPI:
		drv_data->dpi_changed++;
		break;
	case PULSAR_EVENT_PROFILE:
		drv_data->profile_changed++;
		break;
	case PULSAR_EVENT_POWER:
		/* of no interest */
		break;
	default:
		log_bug_libratbag(device->ratbag,
			"%s: unknown event type %02x\n", __func__,
			(int)payload->data[4]);
	}
}

static bool
pulsar_payload_filter(uint8_t *buf, size_t len)
{
	return len == PULSAR_PACKET_SIZE;
}

static int
pulsar_read_response(const struct ratbag_device *device,
	struct pulsar_payload *payload)
{
	for (;;) {
		const int rc = ratbag_hidraw_read_input_report(device,
			(uint8_t *)payload, sizeof(*payload),
			pulsar_payload_filter);
		if (rc < 0)
			return rc;

		if (payload->cmd == PULSAR_CMD_DEVICE_EVENT) {
			pulsar_count_event(device, payload);
			continue;
		}

		if (!pulsar_is_payload_ok(payload)) {
			log_error(device->ratbag,
				"%s: bad checksum on response\n", __func__);
			return -EIO;
		}

		return 0;
	}
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

/* ----- device commands ----- */
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
pulsar_read_info(struct ratbag_device *device,
	uint8_t device_info[PULSAR_INFO_SIZE])
{
	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	request.data[3] = PULSAR_INFO_SIZE * 2; // len

	for (size_t i = 0; i < PULSAR_INFO_SIZE; i++)
		request.data[i + PULSAR_INFO_SIZE] = rand();

	pulsar_finalize_payload(&request, PULSAR_CMD_INFO);
	const int rc = pulsar_transaction(device, &request, &response);
	if (rc < 0)
		return rc;

	memcpy(device_info, response.data + 4 + PULSAR_INFO_SIZE,
		PULSAR_INFO_SIZE);

	/*
	 * Verify challenge-response. Response layout from offset 6:
	 *   [0..3] encoded response
	 *   [4..7] device info (ID + conn type)
	 *
	 * resp[i] = challenge[i] * (i+1) + challenge[(i+1) % 4] + device_id[i]
	 *
	 * bytes 6..7 are zeroed for verification.
	 */
	response.data[6 + PULSAR_INFO_SIZE] = 0;
	response.data[7 + PULSAR_INFO_SIZE] = 0;

	for (size_t i = 0; i < PULSAR_INFO_SIZE; i++) {
		const uint8_t expected = response.data[4 + PULSAR_INFO_SIZE + i];
		const uint8_t actual = response.data[4 + i] -
			(i + 1) * request.data[4 + i] -
			request.data[4 + (i + 1) % PULSAR_INFO_SIZE];

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

	int active_profile = response.data[4];
	if (active_profile >= PULSAR_NUM_PROFILES)
		return -EIO;

	return response.data[4];
}

static int
pulsar_write_active_profile(struct ratbag_device *device, uint8_t index)
{
	log_debug(device->ratbag, "%s: index=%d\n", __func__, (int)index);

	if (index >= PULSAR_NUM_PROFILES)
		return -1;

	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	request.data[3] = 0x01;
	request.data[4] = (uint8_t)index;
	pulsar_finalize_payload(&request, PULSAR_CMD_ACTIVE_PROFILE_SET);
	int rc = pulsar_transaction(device, &request, &response);
	if (rc < 0)
		return rc;

#if DRV_VERIFY_PROFILE == 1
	rc = pulsar_read_active_profile(device);
	if (rc < 0)
		return rc;

	if (rc != index) {
		log_bug_libratbag(device->ratbag,
			"%s: active profile was not set\n", __func__);
	}
#endif

	return 0;
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
		const uint16_t batch_addr = addr + done;
		const size_t batch_len = min(len - done, PULSAR_MEM_BATCH);

		const int rc = pulsar_read_memory(device, batch_addr,
			buf + done, batch_len);
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
	assert(len <= PULSAR_MEM_BATCH);

	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	request.data[1] = (uint8_t)(addr >> 8);
	request.data[2] = (uint8_t)(addr & 0xff);
	request.data[3] = (uint8_t)len;
	memcpy(request.data + 4, data, len);
	pulsar_finalize_payload(&request, PULSAR_CMD_MEM_SET);

	return pulsar_transaction(device, &request, &response);
}

static int
pulsar_write_memory_area(struct ratbag_device *device, uint16_t addr,
			 const uint8_t *data, size_t len)
{
	size_t done = 0;

	while (done < len) {
		const uint16_t batch_addr = addr + done;
		const size_t batch_len = min(len - done, PULSAR_MEM_BATCH);

		const int rc = pulsar_write_memory(device, batch_addr,
			data + done, batch_len);
		if (rc < 0)
			return rc;

		done += batch_len;
	}

	return 0;
}

static int
pulsar_write_setting_byte(struct ratbag_device *device, uint16_t addr,
			  uint8_t value)
{
	uint8_t data[2];

	data[0] = value;
	data[1] = (PULSAR_CHECKSUM_MAGIC - value) & 0xFF;

	return pulsar_write_memory(device, addr, data, 2);
}

static int
pulsar_write_dpi(struct ratbag_device *device, unsigned int mode_index,
		 unsigned int dpi)
{
	uint8_t data[4];
	const uint16_t addr = PULSAR_ADDR_DPI_BASE + mode_index * 4;

	const int rc = pulsar_encode_dpi(dpi, data);
	if (rc < 0)
		return rc;
	data[3] = pulsar_checksum(data, sizeof(data) - 1);

	return pulsar_write_memory(device, addr, data, 4);
}

static int
pulsar_write_dpi_color(struct ratbag_device *device, unsigned int mode_index,
		       struct ratbag_color color)
{
	uint8_t data[4];
	const uint16_t addr = PULSAR_ADDR_DPI_COLOR_BASE + mode_index * 4;

	log_debug(device->ratbag,
		"pulsar_write_dpi_color index=%u color=%02x%02x%02x\n",
		mode_index, color.red, color.green, color.blue);

	data[0] = color.red;
	data[1] = color.green;
	data[2] = color.blue;
	data[3] = pulsar_checksum(data, sizeof(data) - 1);

	return pulsar_write_memory(device, addr, data, 4);
}

static int
pulsar_write_led(struct ratbag_device *device, const struct ratbag_led *led)
{
	uint8_t effect;
	uint8_t enabled;

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

	int rc = pulsar_write_setting_byte(
		device, PULSAR_ADDR_LED_ENABLED, enabled);
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
		int ms = led->ms;
		if (ms < MIN_BREATHE_PERIOD_MS) ms = MIN_BREATHE_PERIOD_MS;
		if (ms > MAX_BREATHE_PERIOD_MS) ms = MAX_BREATHE_PERIOD_MS;
		unsigned int speed = (BREATHE_SPEED_OFFSET - ms +
				      BREATHE_SPEED_SLOPE / 2) /
				     BREATHE_SPEED_SLOPE;

		rc = pulsar_write_setting_byte(device,
					       PULSAR_ADDR_LED_BREATHE_SPEED,
					       speed);
		if (rc)
			return rc;
	}

	return 0;
}

static int
pulsar_read_combination (struct ratbag_device *device,
	struct pulsar_combination *combination, size_t index, size_t profile)
{
	const uint16_t addr = PULSAR_COMB_BASE_ADDR +
		index * sizeof(*combination);

	log_debug(device->ratbag, "%s: profile=%lu index=%lu\n", __func__,
		profile, index);

	int ret = pulsar_read_memory_area(device, addr, (uint8_t *)combination,
		sizeof(*combination));
	if (ret < 0)
		return ret;

	// empty combination
	if (combination->count == 0 || combination->count == 0xFF) {
		combination->count = 0;
		return 0;
	}

	// invalid count
	if (combination->count > PULSAR_COMB_NUM_ACTIONS) {
		log_error(device->ratbag,
			"invalid combination count %u profile=%lu index=%lu\n",
			combination->count, profile, index);
		combination->count = 0;
		return -EIO;
	}

	// verify checksum (covers count byte + used actions only)
	const size_t chk_len = 1 + combination->count *
		sizeof(*combination->actions);
	const uint8_t expected = pulsar_checksum((uint8_t *)combination,
		chk_len);
	const uint8_t actual = ((uint8_t *)combination)[chk_len];
	if (expected != actual) {
		log_error(device->ratbag,
			"invalid combination checksum %02x != %02x profile=%lu index=%lu\n",
			expected, actual, profile, index);
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

	const uint16_t addr = PULSAR_MACRO_BASE_ADDR + index * sizeof(*macro);
	const ptrdiff_t action_offset = sizeof(macro->name_length) +
		sizeof(macro->name) + sizeof(macro -> num_actions);

	// name and action count
	int ret = pulsar_read_memory_area(device, addr, (uint8_t *)macro,
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
	const size_t action_size = sizeof(*macro->actions) *
		min(macro->num_actions, PULSAR_MACRO_NUM_ACTIONS) + 1;

	ret = pulsar_read_memory_area(device, addr + action_offset,
		(uint8_t *) macro + action_offset, action_size);
	if (ret < 0)
		return ret;

	// verify checksum (covers action section only: num_actions + actions)
	const size_t chk_offset = offsetof(struct pulsar_macro, num_actions);
	const size_t chk_len = 1 + sizeof(*macro->actions) *
		min(macro->num_actions, PULSAR_MACRO_NUM_ACTIONS);
	const uint8_t expected = pulsar_checksum(
		(uint8_t *)macro + chk_offset, chk_len);
	const uint8_t checksum = *((uint8_t *)macro + chk_offset + chk_len);
	if (expected != checksum) {
		log_error(device->ratbag,
			"invalid macro checksum %02x != %02x profile=%lu index=%lu\n",
			expected, checksum, profile, index);
		macro->num_actions = 0;
	}

	return 0;
}

static int
pulsar_read_active_dpi_mode (struct ratbag_device *device) {
	uint8_t data[2];
	const int rc = pulsar_read_memory(device, PULSAR_ADDR_ACTIVE_DPI_MODE,
		data, sizeof(data));
	if (rc < 0)
		return rc;

	if (!pulsar_verify_setting(data, 0, 1))
		return -EIO;

	if (data[0] >= PULSAR_NUM_DPI_MODES)
		return -EIO;

	return data[0];
}

/* ----- button mode parsing helpers ----- */

static void
pulsar_parse_btn_mouse(const struct ratbag_profile *profile,
		       struct ratbag_button *button, uint8_t param1)
{
	switch (param1) {
	case PULSAR_MOUSE_LEFT:
		button->action.action.button = 1;
		break;
	case PULSAR_MOUSE_RIGHT:
		button->action.action.button = 2;
		break;
	case PULSAR_MOUSE_MIDDLE:
		button->action.action.button = 3;
		break;
	case PULSAR_MOUSE_BACK:
		button->action.action.button = 4;
		break;
	case PULSAR_MOUSE_FORWARD:
		button->action.action.button = 5;
		break;
	default:
		log_bug_libratbag(profile->device->ratbag,
			"%s: button %u: unknown mouse param %02x\n",
			__func__, button->index, param1);
		button->action.type = RATBAG_BUTTON_ACTION_TYPE_UNKNOWN;
		return;
	}

	button->action.type = RATBAG_BUTTON_ACTION_TYPE_BUTTON;
}

static void
pulsar_parse_btn_dpi(const struct ratbag_profile *profile,
		     struct ratbag_button *button, uint8_t param1)
{
	switch (param1) {
	case PULSAR_DPI_CYCLE:
		button->action.action.special =
			RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_CYCLE_UP;
		break;
	case PULSAR_DPI_UP:
		button->action.action.special =
			RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_UP;
		break;
	case PULSAR_DPI_DOWN:
		button->action.action.special =
			RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_DOWN;
		break;
	default:
		log_bug_libratbag(profile->device->ratbag,
			"%s: button %u: unknown DPI param %02x\n",
			__func__, button->index, param1);
		button->action.type = RATBAG_BUTTON_ACTION_TYPE_UNKNOWN;
		return;
	}

	button->action.type = RATBAG_BUTTON_ACTION_TYPE_SPECIAL;
}

/**
 * Decode a single pulsar combination/macro action into macro events.
 * Returns the number of events appended.
 */
static unsigned int
pulsar_decode_action(const struct ratbag_device *device,
		     struct ratbag_button_macro *m, unsigned int ei,
		     uint8_t code, uint16_t value, unsigned int button_index)
{
	unsigned int count = 0;
	enum ratbag_macro_event_type etype =
		(code & 0x80) ? RATBAG_MACRO_EVENT_KEY_PRESSED
			      : RATBAG_MACRO_EVENT_KEY_RELEASED;

	switch (code) {
	case PULSAR_ACTION_MOD_PRESS:
	case PULSAR_ACTION_MOD_RELEASE:
		for (size_t mi = 0; mi < ARRAY_LENGTH(pulsar_mod_map); mi++) {
			if (value & pulsar_mod_map[mi].bit) {
				ratbag_button_macro_set_event(
					m, ei + count, etype,
					pulsar_mod_map[mi].key);
				count++;
			}
		}
		break;
	case PULSAR_ACTION_KEY_PRESS:
	case PULSAR_ACTION_KEY_RELEASE: {
		unsigned int keycode =
			ratbag_hidraw_get_keycode_from_keyboard_usage(
				device, (uint8_t)value);
		if (keycode) {
			ratbag_button_macro_set_event(
				m, ei + count, etype, keycode);
			count++;
		}
		break;
	}
	case PULSAR_ACTION_CONSUMER_PRESS:
	case PULSAR_ACTION_CONSUMER_RELEASE: {
		unsigned int keycode =
			ratbag_hidraw_get_keycode_from_consumer_usage(
				device, value);
		if (keycode) {
			ratbag_button_macro_set_event(
				m, ei + count, etype, keycode);
			count++;
		}
		break;
	}
	case PULSAR_ACTION_MOUSE_PRESS:
	case PULSAR_ACTION_MOUSE_RELEASE:
		for (size_t mi = 0; mi < ARRAY_LENGTH(pulsar_mouse_map); mi++) {
			if (value & pulsar_mouse_map[mi].bit) {
				ratbag_button_macro_set_event(
					m, ei + count, etype,
					pulsar_mouse_map[mi].keycode);
				count++;
			}
		}
		break;
	default:
		log_bug_libratbag(device->ratbag,
			"%s: button %u: unknown action code %02x\n",
			__func__, button_index, code);
		break;
	}

	return count;
}

static void
pulsar_parse_btn_combo(const struct ratbag_profile *profile,
		       struct ratbag_button *button,
		       const struct pulsar_combination *comb)
{
	unsigned int bi = button->index;

	if (comb->count == 0 || comb->count > PULSAR_COMB_NUM_ACTIONS) {
		log_error(profile->device->ratbag,
			"%s: button %u: invalid combo count %u\n",
			__func__, bi, comb->count);
		button->action.type = RATBAG_BUTTON_ACTION_TYPE_UNKNOWN;
		return;
	}

	struct ratbag_button_macro *m = ratbag_button_macro_new("combo");
	unsigned int ei = 0;

	for (uint8_t ci = 0; ci < comb->count; ci++) {
		const uint8_t code = comb->actions[ci].code;
		const uint16_t value = le16toh(comb->actions[ci].value);

		ei += pulsar_decode_action(profile->device, m, ei,
					   code, value, bi);
	}

	ratbag_button_copy_macro(button, m);
	ratbag_button_macro_unref(m);
}

static void
pulsar_parse_btn_macro(const struct ratbag_profile *profile,
		       struct ratbag_button *button,
		       const struct pulsar_macro *mac,
		       uint8_t repeat_param)
{
	const unsigned int bi = button->index;
	struct ratbag_button_macro *m = ratbag_button_macro_new("macro");
	unsigned int ei = 0;

	for (uint8_t ai = 0; ai < mac->num_actions; ai++) {
		const uint8_t code = mac->actions[ai].code;
		const uint16_t value = le16toh(mac->actions[ai].value);
		const uint16_t delay = be16toh(mac->actions[ai].delay);

		ei += pulsar_decode_action(profile->device, m, ei,
					   code, value, bi);

		if (delay > 0) {
			ratbag_button_macro_set_event(
				m, ei++, RATBAG_MACRO_EVENT_WAIT, delay);
		}
	}

	switch (repeat_param) {
	case 0xFE:
		ratbag_button_macro_set_repeat(m,
			RATBAG_MACRO_REPEAT_WHILE_HELD, 0);
		break;
	case 0xFF:
		ratbag_button_macro_set_repeat(m,
			RATBAG_MACRO_REPEAT_UNTIL_BUTTON_PRESSED, 0);
		break;
	default:
		if (repeat_param > 1)
			ratbag_button_macro_set_repeat(m,
				RATBAG_MACRO_REPEAT_COUNT, repeat_param);
		else
			ratbag_button_macro_set_repeat(m,
				RATBAG_MACRO_REPEAT_ONCE, 0);
		break;
	}

	ratbag_button_copy_macro(button, m);
	ratbag_button_macro_unref(m);
}

/* ----- profile settings ----- */

static int
pulsar_read_profile_settings(struct ratbag_profile *profile)
{
	log_debug(profile->device->ratbag, "%s: index=%u\n", __func__,
		profile->index);

	const int active_profile = pulsar_read_active_profile(profile->device);
	if (active_profile < 0)
		return active_profile;

	struct pulsar_data *drv_data = profile->drv_data;
	assert(drv_data != NULL);
	int ret = 0;
	size_t profile_changed_before = drv_data->profile_changed;
	const size_t dpi_changed_before = drv_data->dpi_changed;

	/* switch to the target profile if needed */
	if ((unsigned)active_profile != profile->index) {
		ret = pulsar_write_active_profile(profile->device, profile->index);
		if (ret < 0)
			return ret;
		++profile_changed_before;
	}

	/* read settings */
	uint8_t *settings = drv_data->memory[profile->index].settings;
	ret = pulsar_read_memory_area(profile->device, 0, settings,
		PULSAR_SETTINGS_SIZE);
	if (ret < 0)
		goto out;

	/* read combinations */
	for (size_t i = 0; i < PULSAR_NUM_BUTTONS_X2A; i++) {
		ret = pulsar_read_combination(profile->device,
			drv_data->memory[profile->index].combination + i,
			i, profile->index);
		if (ret < 0)
			goto out;
	}

	/* read macros */
	for (size_t i = 0; i < PULSAR_NUM_MACROS; i++) {
		ret = pulsar_read_macro(profile->device,
			drv_data->memory[profile->index].macro + i,
			i, profile->index);
		if (ret < 0)
			goto out;
	}

	bool state_changed = false;

	/* check if the profile was changed during read */
	/* occurs if user presses the profile change button */
	if (drv_data->profile_changed != profile_changed_before) {
		log_error(profile->device->ratbag,
			"profile changed during read, aborting (%zu -> %zu)\n",
			profile_changed_before, drv_data->profile_changed);
		state_changed = true;
	}

	/* check if dpi was changed during read */
	/* occurs if user presses the dpi change button */
	if (drv_data->dpi_changed != dpi_changed_before) {
		log_error(profile->device->ratbag,
			"dpi changed during read, aborting (%zu -> %zu)\n",
			dpi_changed_before, drv_data->dpi_changed);
		state_changed = true;
	}

	/* restore active profile */
	if ((unsigned) active_profile != profile->index)
	{
		ret = pulsar_write_active_profile(profile->device, active_profile);
		if (ret < 0) {
			log_error(profile->device->ratbag,
				"failed to restore active profile (%d)\n", ret);
			// parse settings even if profile restore failed
		} else {
			log_debug(profile->device->ratbag,
				"active profile restored\n");
		}
	}

	if (state_changed) {
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
			ret = -EIO;
			goto out;
		}
	}

	/* verify DPI value checksums (3 bytes + checksum) */
	const uint8_t dpi_count = settings[PULSAR_ADDR_DPI_MODE_COUNT];
	for (unsigned int i = 0; i < PULSAR_NUM_DPI_MODES; i++) {
		const uint16_t dpi_addr = PULSAR_ADDR_DPI_BASE + i * 4;
		if (!pulsar_verify_setting(settings, dpi_addr, 3)) {
			log_error(profile->device->ratbag,
				"invalid checksum for dpi mode %u at 0x%04x\n",
				i, dpi_addr);
			ret = -EIO;
			goto out;
		}

		const uint16_t color_addr = PULSAR_ADDR_DPI_COLOR_BASE + i * 4;
		if (!pulsar_verify_setting(settings, color_addr, 3)) {
			log_error(profile->device->ratbag,
				"invalid checksum for dpi color %u at 0x%04x\n",
				i, color_addr);
			ret = -EIO;
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
		const uint16_t dpi_addr =
			PULSAR_ADDR_DPI_BASE + resolution->index * 4;
		const unsigned int dpi =
			pulsar_decode_dpi(settings + dpi_addr);
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

	/* lift off distance */
	profile->lod = (double)settings[PULSAR_ADDR_LOD];
	log_debug(profile->device->ratbag, "  lod: %.1f mm\n",
		profile->lod);

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

			if (speed >= MIN_BREATH_SPEED && speed <= MAX_BREATH_SPEED)
				led->ms = BREATHE_SPEED_OFFSET - BREATHE_SPEED_SLOPE * speed;
			else
				led->ms = (MIN_BREATHE_PERIOD_MS + MAX_BREATHE_PERIOD_MS) / 2;

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

	/* motion sync */
	profile->motion_sync = settings[PULSAR_ADDR_MOTION_SYNC];
	log_debug(profile->device->ratbag, "  motion sync: %s\n",
		profile->motion_sync ? "on" : "off");

	/* ripple control */
	profile->ripple_control = settings[PULSAR_ADDR_RIPPLE_CONTROL];
	log_debug(profile->device->ratbag, "  ripple control: %s\n",
		profile->ripple_control ? "on" : "off");

	/* autosleep (raw value * 10 = seconds) */
	profile->autosleep = settings[PULSAR_ADDR_AUTOSLEEP] * 10;
	log_debug(profile->device->ratbag, "  autosleep: %d s\n",
		profile->autosleep);

	/* buttons */
	struct ratbag_button *button;
	ratbag_profile_for_each_button(profile, button) {
		unsigned int bi = button->index;
		unsigned int addr = PULSAR_ADDR_BUTTON_BASE + bi * 4;
		uint8_t mode   = settings[addr];
		uint8_t param1 = settings[addr + 1];
		uint8_t param2 = settings[addr + 2];

		log_debug(profile->device->ratbag,
			  "  button %u: mode=%02x param1=%02x param2=%02x\n",
			  bi, mode, param1, param2);

		switch (mode) {
		case PULSAR_BTN_MODE_DISABLED:
			button->action.type = RATBAG_BUTTON_ACTION_TYPE_NONE;
			break;
		case PULSAR_BTN_MODE_MOUSE:
			pulsar_parse_btn_mouse(profile, button, param1);
			break;
		case PULSAR_BTN_MODE_DPI:
			pulsar_parse_btn_dpi(profile, button, param1);
			break;
		case PULSAR_BTN_MODE_PROFILE:
			button->action.type = RATBAG_BUTTON_ACTION_TYPE_SPECIAL;
			button->action.action.special =
				RATBAG_BUTTON_ACTION_SPECIAL_PROFILE_CYCLE_UP;
			break;
		case PULSAR_BTN_MODE_DPI_LOCK: {
			unsigned int dpi = (unsigned)(param1 + 1) * 50;
			log_debug(profile->device->ratbag,
				  "  button %u: DPI lock %u\n", bi, dpi);
			button->action.type = RATBAG_BUTTON_ACTION_TYPE_DPI_LOCK;
			button->action.action.dpi_lock.x = dpi;
			button->action.action.dpi_lock.y = dpi;
			break;
		}
		case PULSAR_BTN_MODE_COMBO:
			pulsar_parse_btn_combo(profile, button,
				&drv_data->memory[profile->index].combination[bi]);
			break;
		case PULSAR_BTN_MODE_MACRO: {
			uint8_t slot = param1;
			if (slot >= PULSAR_NUM_MACROS) {
				log_bug_libratbag(profile->device->ratbag,
					"%s: button %u: invalid macro slot %u\n",
					__func__, bi, slot);
				button->action.type = RATBAG_BUTTON_ACTION_TYPE_UNKNOWN;
				break;
			}
			pulsar_parse_btn_macro(profile, button,
				&drv_data->memory[profile->index].macro[slot],
				param2);
			break;
		}
		default:
			log_error(profile->device->ratbag,
				  "  button %u: unknown mode %02x\n",
				  bi, mode);
			button->action.type = RATBAG_BUTTON_ACTION_TYPE_UNKNOWN;
			break;
		}
	}

out:
	return ret;
}

/* ----- ratbag callbacks ----- */

static int
pulsar_test_hidraw(struct ratbag_device *device)
{
	int rc = ratbag_hidraw_has_report(device, PULSAR_REPORT_ID);
	log_debug(device->ratbag,
		  "%s: hidraw[0] fd=%d sysname=%s has_report(0x%02x)=%d\n",
		  __func__, device->hidraw[0].fd,
		  device->hidraw[0].sysname ? device->hidraw[0].sysname : "(null)",
		  PULSAR_REPORT_ID, rc);
	return rc;
}

static bool
is_x2a(const uint8_t data[PULSAR_INFO_SIZE])
{
	return data[0] == 0x06 && (data[1] == 0x08 || data[1] == 0x09);
}

static int
pulsar_probe(struct ratbag_device *device)
{
	const int polling =
		ratbag_device_data_pulsar_get_polling(device->data) * 1000;
	if (polling != 1000 && polling != 4000) {
		log_bug_libratbag(device->ratbag,
			"%s: invalid driver config: polling %d is not supported\n",
			__func__, polling);
		return -EINVAL;
	}

	int rc = ratbag_find_hidraw(device, pulsar_test_hidraw);
	if (rc) {
		return rc;
	}

	log_debug(device->ratbag,
		  "%s: hidraw[0] fd=%d sysname=%s\n", __func__,
		  device->hidraw[0].fd,
		  device->hidraw[0].sysname ? device->hidraw[0].sysname : "(null)");

	struct pulsar_data *drv_data = zalloc(sizeof(*drv_data));
	ratbag_set_drv_data(device, drv_data);

	// read device info
	uint8_t device_info[PULSAR_INFO_SIZE];
	rc = pulsar_read_info(device, device_info);
	if (rc < 0) {
		log_error(device->ratbag,
			"%s: failed to read device info (rc=%d)\n", __func__, rc);
		goto err_info;
	}
	log_buf_debug(device->ratbag, "pulsar device info: ", device_info,
		PULSAR_INFO_SIZE);

	// verify polling against device info
	if (polling == 4000 && device_info[2] != 0x01) {
		log_bug_libratbag(device->ratbag,
			"%s: 4kHz polling without 4k dongle\n", __func__);
		goto err_info;
	}

	const unsigned int num_buttons =
		is_x2a(device_info) ? PULSAR_NUM_BUTTONS_X2A : PULSAR_NUM_BUTTONS;

	rc = ratbag_device_init_profiles(device, PULSAR_NUM_PROFILES,
					 PULSAR_NUM_DPI_MODES,
					 num_buttons,
					 PULSAR_NUM_LEDS);
	if (rc < 0)
		goto err_init;

	const size_t num_rates = polling == 4000 ? 6 : 4;
	unsigned int * report_rates = calloc(num_rates, sizeof(*report_rates));
	unsigned rate = 125;
	for (size_t i = 0; i < num_rates; i++) {
		report_rates[i] = rate;
		rate <<= 1;
	}

	unsigned int debounce_values[PULSAR_NUM_DEBOUNCE];
	for (unsigned int i = 0; i < PULSAR_NUM_DEBOUNCE; i++)
		debounce_values[i] = i + PULSAR_MIN_DEBOUNCE;

	const double lod_values[] = { 1.0, 2.0 };

	/* autosleep: value * 10 = seconds, range 10-600s (1-60 raw) */
	unsigned int autosleep_values[60];
	for (unsigned int i = 0; i < ARRAY_LENGTH(autosleep_values); i++)
		autosleep_values[i] = (i + 1) * 10;

	struct ratbag_profile *profile;
	ratbag_device_for_each_profile(device, profile)
	{
		profile->drv_data = drv_data;
		profile->is_enabled = true;

		ratbag_profile_set_report_rate_list(profile, report_rates,
						    num_rates);

		ratbag_profile_set_debounce_list(profile, debounce_values,
			ARRAY_LENGTH(debounce_values));

		ratbag_profile_set_lod_list(profile, lod_values,
					    ARRAY_LENGTH(lod_values));

		ratbag_profile_set_autosleep_list(profile, autosleep_values,
		ARRAY_LENGTH(autosleep_values));

		struct ratbag_resolution *resolution;
		ratbag_profile_for_each_resolution(profile, resolution) {
			ratbag_resolution_set_dpi_list_from_range(resolution,
				PULSAR_DPI_MIN, PULSAR_DPI_MAX);
			ratbag_resolution_set_cap(resolution,
						  RATBAG_RESOLUTION_CAP_DISABLE);
		}

		struct ratbag_button *button;
		ratbag_profile_for_each_button(profile, button) {
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_NONE);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_BUTTON);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_SPECIAL);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_MACRO);
			ratbag_button_enable_action_type(button, RATBAG_BUTTON_ACTION_TYPE_DPI_LOCK);
		}

		struct ratbag_led *led;
		ratbag_profile_for_each_led(profile, led) {
			if (led->index == 0) {
				/* LED 0: global effect (on/off/breathing) */
				ratbag_led_set_mode_capability(led, RATBAG_LED_OFF);
				ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
				ratbag_led_set_mode_capability(led, RATBAG_LED_BREATHING);
				led->colordepth = RATBAG_LED_COLORDEPTH_MONOCHROME;
			} else {
				/* LEDs 1-8: per-DPI-mode color */
				ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
				led->colordepth = RATBAG_LED_COLORDEPTH_RGB_888;
			}
		}
	}

	free(report_rates);

	// check if device is alive
	size_t retries = DRV_INIT_RETRY;
	do {
		rc = pulsar_read_status(device);
		if (rc < 0)
			goto err_init;
		if (rc || !retries--)
			break;
		sleep(DRV_RETRY_DELAY_S);
	} while (true);


	if (rc == 0) {
		log_info(device->ratbag,
			 "%s device not responding, deferring profile reads\n",
			 __func__);
		drv_data->active_profile = 0;
		ratbag_device_for_each_profile(device, profile) {
			profile->is_active = (profile->index == 0);
		}
		return 0;
	}

	int active_profile = pulsar_read_active_profile(device);
	if (active_profile < 0)
		goto err_init;

	drv_data->active_profile = active_profile;
	ratbag_device_for_each_profile(device, profile) {
		profile->is_active = (profile->index == (unsigned)active_profile);
		if (profile->is_active) {
			rc = pulsar_read_profile_settings(profile);
			if (rc < 0)
				goto err_init;;
			profile->loaded = true;
		}
	}

	return 0;

err_init:
	free(drv_data);
err_info:
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

static bool
pulsar_macro_is_combo(const struct ratbag_device *device,
		      const struct ratbag_macro *macro)
{
	if (macro->repeat_mode == RATBAG_MACRO_REPEAT_COUNT &&
	    macro->repeat_count != 1)
		return false;
	if (macro->repeat_mode != RATBAG_MACRO_REPEAT_ONCE &&
	    macro->repeat_mode != RATBAG_MACRO_REPEAT_COUNT)
		return false;

	unsigned int count = 0;
	unsigned int presses = 0;

	for (unsigned int i = 0; i < MAX_MACRO_EVENTS; i++) {
		switch (macro->events[i].type) {
		case RATBAG_MACRO_EVENT_KEY_PRESSED:
			presses++;
			// fallthrough
		case RATBAG_MACRO_EVENT_KEY_RELEASED:
			if (macro->events[i].event.key >= BTN_MOUSE &&
			    macro->events[i].event.key <= BTN_TASK) {
				log_info(device->ratbag,
					"macro is not a combo: mouse key %u at event %u\n",
					macro->events[i].event.key, i);
				return false;
			}
			count++;
			break;
		case RATBAG_MACRO_EVENT_WAIT:
			log_info(device->ratbag,
				"macro is not a combo: delay at event %u\n", i);
			return false;
		case RATBAG_MACRO_EVENT_NONE:
		case RATBAG_MACRO_EVENT_INVALID:
			if (count == 0) {
				log_info(device->ratbag,
					"macro is not a combo: no actions\n");
				return false;
			}
			if (count > PULSAR_COMB_NUM_ACTIONS) {
				log_info(device->ratbag,
					"macro is not a combo: %u actions exceeds limit of %u\n",
					count, PULSAR_COMB_NUM_ACTIONS);
				return false;
			}
			return true;
		}
	}

	if (count == 0 || count > PULSAR_COMB_NUM_ACTIONS ||
	    presses != (count - presses)) {
		log_info(device->ratbag,
			"macro is not a combo: %u actions, %u presses\n",
			count, presses);
		return false;
	}

	return true;
}

static bool
pulsar_macro_is_valid(const struct ratbag_device *device,
		      const struct ratbag_macro *macro)
{
	unsigned int actions = 0;
	unsigned int pressed[PULSAR_MACRO_NUM_ACTIONS / 2];
	unsigned int num_pressed = 0;
	unsigned int pending_delay = 0;

	for (unsigned int i = 0; i < MAX_MACRO_EVENTS; i++) {
		const struct ratbag_macro_event *ev = &macro->events[i];

		switch (ev->type) {
		case RATBAG_MACRO_EVENT_KEY_PRESSED:
			for (unsigned int j = 0; j < num_pressed; j++) {
				if (pressed[j] == ev->event.key) {
					log_error(device->ratbag,
						"invalid macro: key %u pressed again at event %u\n",
						ev->event.key, i);
					return false;
				}
			}
			if (num_pressed >= ARRAY_LENGTH(pressed)) {
				log_error(device->ratbag,
					"invalid macro: too many keys pressed simultaneously at event %u\n",
					i);
				return false;
			}
			pressed[num_pressed++] = ev->event.key;
			actions++;
			pending_delay = 0;
			if (ratbag_hidraw_get_consumer_usage_from_keycode(
				    device, ev->event.key)) {
				log_error(device->ratbag,
					"invalid macro: consumer key %u at event %u\n",
					ev->event.key, i);
				return false;
			}
			break;
		case RATBAG_MACRO_EVENT_KEY_RELEASED: {
			bool found = false;
			for (unsigned int j = 0; j < num_pressed; j++) {
				if (pressed[j] == ev->event.key) {
					pressed[j] = pressed[--num_pressed];
					found = true;
					break;
				}
			}
			if (!found) {
				log_error(device->ratbag,
					"invalid macro: key %u released without press at event %u\n",
					ev->event.key, i);
				return false;
			}
			actions++;
			pending_delay = 0;
			if (ratbag_hidraw_get_consumer_usage_from_keycode(
				    device, ev->event.key)) {
				log_error(device->ratbag,
					"invalid macro: consumer key %u at event %u\n",
					ev->event.key, i);
				return false;
			}
			break;
		}
		case RATBAG_MACRO_EVENT_WAIT:
			pending_delay += ev->event.timeout;
			if (pending_delay > UINT16_MAX) {
				log_error(device->ratbag,
					"invalid macro: cumulative delay %u exceeds limit at event %u\n",
					pending_delay, i);
				return false;
			}
			break;
		case RATBAG_MACRO_EVENT_NONE:
		case RATBAG_MACRO_EVENT_INVALID:
			break;
		}
	}

	if (actions == 0) {
		log_error(device->ratbag,
			"%s: invalid macro: no actions\n", __func__);
		return false;
	}

	if (actions > PULSAR_MACRO_NUM_ACTIONS) {
		log_error(device->ratbag,
			"%s: invalid macro: %u actions exceeds limit of %u\n",
			__func__, actions, PULSAR_MACRO_NUM_ACTIONS);
		return false;
	}

	if (num_pressed > 0) {
		log_error(device->ratbag,
			"%s: invalid macro: %u key(s) still pressed at end\n",
			__func__, num_pressed);
		return false;
	}

	return true;
}

static int
pulsar_check_macro(const struct ratbag_device *device,
	const struct ratbag_profile *profile,
	const struct ratbag_button *button)
{
	if (!button->action.macro) {
		log_error(device->ratbag,
			"%s: profile %u: button %d: macro action with no macro data\n",
			__func__, profile->index, button->index);
		return RATBAG_ERROR_VALUE;
	}

	const struct ratbag_macro *macro = button->action.macro;
	if (pulsar_macro_is_combo(device, macro))
		return 0;

	if (!pulsar_macro_is_valid(device, macro))
		return RATBAG_ERROR_VALUE;

	switch (macro->repeat_mode) {
	case RATBAG_MACRO_REPEAT_ONCE:
	case RATBAG_MACRO_REPEAT_WHILE_HELD:
	case RATBAG_MACRO_REPEAT_UNTIL_BUTTON_PRESSED:
		break;
	case RATBAG_MACRO_REPEAT_COUNT:
		if (macro->repeat_count == 0 || macro->repeat_count > 0xFD) {
			log_error(device->ratbag,
				"%s: invalid macro repeat count %u (valid: 1-253)\n",
				__func__, macro->repeat_count);
			return RATBAG_ERROR_VALUE;
		}
		break;
	default:
		log_error(device->ratbag,
			"%s: unsupported macro repeat mode %d\n",
			__func__, macro->repeat_mode);
		return RATBAG_ERROR_VALUE;
	}

	return 0;
}

static int
pulsar_check_button(const struct ratbag_device *device,
	const struct ratbag_profile *profile,
	struct ratbag_button *button)
{
	if (!button->dirty)
		return 0;

	log_info(device->ratbag,
		"  button %d dirty: action type=%d\n",
		button->index, button->action.type);

	switch (button->action.type) {
	case RATBAG_BUTTON_ACTION_TYPE_NONE:
		break;
	case RATBAG_BUTTON_ACTION_TYPE_BUTTON:
		if (button->action.action.button < 1 ||
		    button->action.action.button > 5) {
			log_error(device->ratbag,
				"%s: button %d: unsupported button %u\n",
				__func__, button->index,
				button->action.action.button);
			return RATBAG_ERROR_VALUE;
		    }
		log_debug(device->ratbag,
			"    mouse button %u\n",
			button->action.action.button);
		break;
	case RATBAG_BUTTON_ACTION_TYPE_SPECIAL:
		switch (button->action.action.special) {
		case RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_CYCLE_UP:
		case RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_UP:
		case RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_DOWN:
		case RATBAG_BUTTON_ACTION_SPECIAL_PROFILE_CYCLE_UP:
			break;
		default:
			log_error(device->ratbag,
				"%s: button %d: unsupported special action %d\n",
				__func__, button->index,
				button->action.action.special);
			return RATBAG_ERROR_VALUE;
		}
		break;
	case RATBAG_BUTTON_ACTION_TYPE_DPI_LOCK: {
		unsigned int dpi = button->action.action.dpi_lock.x;
		if (dpi < 100 || dpi > 1100) {
			log_error(device->ratbag,
				"%s: button %d: unsupported DPI lock value %u\n",
				__func__, button->index, dpi);
			return RATBAG_ERROR_VALUE;
		}
		/* round to nearest step of 100 */
		dpi = ((dpi + 50) / 100) * 100;
		button->action.action.dpi_lock.x = dpi;
		button->action.action.dpi_lock.y = dpi;
		log_debug(device->ratbag,
			"    DPI lock %u\n", dpi);
		break;
	}
	case RATBAG_BUTTON_ACTION_TYPE_MACRO: {
		const int rc = pulsar_check_macro(device, profile, button);
		if (rc < 0)
			return rc;
		}
		break;
	default:
		log_error(device->ratbag,
			"  button %d: unsupported action type %d\n",
			button->index, button->action.type);
		return RATBAG_ERROR_VALUE;
	}

	return 0;
}

static int
pulsar_check_profile(const struct ratbag_device *device,
	struct ratbag_profile *profile)
{
	if (!profile->dirty)
		return 0;

	log_info(device->ratbag,
		"%s: profile %d dirty (active=%d, enabled=%d, hz=%u)\n",
		__func__, profile->index, profile->is_active,
		profile->is_enabled, profile->hz);

	if (profile->rate_dirty) {
		log_info(device->ratbag,
			"  report rate: %u Hz\n", profile->hz);
		if (pulsar_hz_to_polling_rate(profile->hz) == 0)
			return RATBAG_ERROR_VALUE;
	}

	if (profile->angle_snapping_dirty)
		log_info(device->ratbag,
			"  angle snapping: %d\n", profile->angle_snapping);

	if (profile->debounce_dirty) {
		log_info(device->ratbag,
			"  debounce: %d ms\n", profile->debounce);
		if (profile->debounce < PULSAR_MIN_DEBOUNCE ||
		    profile->debounce > PULSAR_MAX_DEBOUNCE)
			return RATBAG_ERROR_VALUE;
	}

	if (profile->lod_dirty) {
		if (profile->lod < 1.0 || profile->lod > 2.0)
			return RATBAG_ERROR_VALUE;
		profile->lod = profile->lod < 1.5 ? 1.0 : 2.0;
		log_info(device->ratbag,
			"  lod: %.1f mm\n", profile->lod);
	}

	if (profile->motion_sync_dirty)
		log_info(device->ratbag,
			"  motion sync: %d\n", profile->motion_sync);

	if (profile->ripple_control_dirty)
		log_info(device->ratbag,
			"  ripple control: %d\n", profile->ripple_control);

	if (profile->autosleep_dirty) {
		if (profile->autosleep < 10 || profile->autosleep > 600)
			return RATBAG_ERROR_VALUE;
		/* round up to the nearest multiple of 10 */
		profile->autosleep =
			((profile->autosleep + 9) / 10) * 10;
		log_info(device->ratbag,
			"  autosleep: %d s\n", profile->autosleep);
	}

	struct ratbag_resolution *resolution;
	ratbag_profile_for_each_resolution(profile, resolution) {
		if (!resolution->dirty)
			continue;
		log_info(device->ratbag,
			"  resolution %d dirty: dpi=%ux%u active=%d disabled=%d\n",
			resolution->index, resolution->dpi_x,
			resolution->dpi_y, resolution->is_active,
			resolution->is_disabled);
		if (resolution->dpi_x < PULSAR_DPI_MIN ||
		    resolution->dpi_x > PULSAR_DPI_MAX)
			return RATBAG_ERROR_VALUE;
		/* round to the nearest step */
		resolution->dpi_x = ((resolution->dpi_x +
			PULSAR_DPI_STEP / 2) / PULSAR_DPI_STEP) *
			PULSAR_DPI_STEP;
		resolution->dpi_y = resolution->dpi_x;
	}

	struct ratbag_button *button;
	ratbag_profile_for_each_button(profile, button) {
		const int rc = pulsar_check_button(device, profile, button);
		if (rc < 0)
			return rc;
	}

	struct ratbag_led *led;
	ratbag_profile_for_each_led(profile, led) {
		if (!led->dirty)
			continue;
		log_info(device->ratbag,
			"  led %d dirty: mode=%d color=%02x%02x%02x brightness=%u ms=%u\n",
			led->index, led->mode,
			led->color.red, led->color.green, led->color.blue,
			led->brightness, led->ms);
	}

	unsigned int highest_enabled = 0;

	/* The device only supports a contiguous range of DPI modes (0 to
	 * dpi_count-1). Re-enable any disabled modes below the highest
	 * enabled one so there are no gaps. */
	ratbag_profile_for_each_resolution(profile, resolution) {
		if (!resolution->is_disabled)
			highest_enabled = resolution->index;
	}

	ratbag_profile_for_each_resolution(profile, resolution) {
		if (resolution->index <= highest_enabled &&
		    resolution->is_disabled) {
			resolution->is_disabled = false;
			log_info(device->ratbag,
				"%s: re-enabling dpi mode %u to fill gap\n",
				__func__, resolution->index);
		    }
	}

	return 0;
}

/**
 * Encode a single macro event into a pulsar combination/macro action triplet.
 * Returns true on success, false if the key cannot be encoded.
 */
static bool
pulsar_encode_action(const struct ratbag_device *device,
		     const struct ratbag_macro_event *ev,
		     uint8_t *code, uint16_t *value)
{
	const bool press = ev->type == RATBAG_MACRO_EVENT_KEY_PRESSED;
	const unsigned int key = ev->event.key;

	/* modifier keys */
	for (size_t i = 0; i < ARRAY_LENGTH(pulsar_mod_map); i++) {
		if (pulsar_mod_map[i].key == key) {
			*code = press ? PULSAR_ACTION_MOD_PRESS
				      : PULSAR_ACTION_MOD_RELEASE;
			*value = htole16(pulsar_mod_map[i].bit);
			return true;
		}
	}

	/* mouse buttons */
	const uint8_t mbtn = pulsar_mouse_btn_from_keycode(key);
	if (mbtn) {
		*code = press ? PULSAR_ACTION_MOUSE_PRESS
			      : PULSAR_ACTION_MOUSE_RELEASE;
		*value = htole16(mbtn);
		return true;
	}

	/* keyboard keys */
	const unsigned int hid =
		ratbag_hidraw_get_keyboard_usage_from_keycode(device, key);
	if (hid) {
		*code = press ? PULSAR_ACTION_KEY_PRESS
			      : PULSAR_ACTION_KEY_RELEASE;
		*value = htole16(hid);
		return true;
	}

	return false;
}

static int
pulsar_write_button_assignment(struct ratbag_device *device,
			       unsigned int button_index,
			       uint8_t mode, uint8_t param1, uint8_t param2)
{
	uint8_t data[4];

	data[0] = mode;
	data[1] = param1;
	data[2] = param2;
	data[3] = pulsar_checksum(data, 3);

	const uint16_t addr = PULSAR_ADDR_BUTTON_BASE + button_index * 4;

	return pulsar_write_memory(device, addr, data, 4);
}

static int
pulsar_commit_btn_combo(struct ratbag_device *device,
			unsigned int button_index,
			const struct ratbag_macro *macro)
{
	struct pulsar_combination comb = {0};
	unsigned int ai = 0;

	for (unsigned int i = 0; i < MAX_MACRO_EVENTS; i++) {
		const struct ratbag_macro_event *ev = &macro->events[i];

		if (ev->type == RATBAG_MACRO_EVENT_NONE ||
		    ev->type == RATBAG_MACRO_EVENT_INVALID)
			break;

		if (ev->type != RATBAG_MACRO_EVENT_KEY_PRESSED &&
		    ev->type != RATBAG_MACRO_EVENT_KEY_RELEASED)
			continue;

		if (ai >= PULSAR_COMB_NUM_ACTIONS)
			return RATBAG_ERROR_VALUE;

		if (!pulsar_encode_action(device, ev,
					  &comb.actions[ai].code,
					  &comb.actions[ai].value))
			return RATBAG_ERROR_VALUE;

		ai++;
	}

	comb.count = ai;
	size_t chk_len = 1 + ai * sizeof(*comb.actions);
	((uint8_t *)&comb)[chk_len] = pulsar_checksum((uint8_t *)&comb,
						       chk_len);

	const uint16_t addr =
		PULSAR_COMB_BASE_ADDR + button_index * sizeof(comb);

	const int rc = pulsar_write_memory_area(device, addr,
		(uint8_t *)&comb, sizeof(comb));
	if (rc < 0)
		return rc;

	return pulsar_write_button_assignment(device, button_index,
					      PULSAR_BTN_MODE_COMBO,
					      0x00, 0x00);
}

static int
pulsar_commit_btn_macro(struct ratbag_device *device,
			unsigned int button_index,
			const struct ratbag_macro *macro)
{
	struct pulsar_macro mac = {0};

	/* encode name */
	const char *name = macro->name ? macro->name : "macro";
	const size_t name_chars = min(strlen(name), 15u);

	for (size_t i = 0; i < name_chars; i++)
		mac.name[i] = htole16((uint16_t)(uint8_t)name[i]);
	mac.name_length = name_chars * 2;

	/* encode actions */
	unsigned int ai = 0;
	unsigned int pending_delay = 0;

	for (unsigned int i = 0; i < MAX_MACRO_EVENTS; i++) {
		const struct ratbag_macro_event *ev = &macro->events[i];

		if (ev->type == RATBAG_MACRO_EVENT_NONE ||
		    ev->type == RATBAG_MACRO_EVENT_INVALID)
			break;

		if (ev->type == RATBAG_MACRO_EVENT_WAIT) {
			pending_delay += ev->event.timeout;
			continue;
		}

		if (ai >= PULSAR_MACRO_NUM_ACTIONS)
			return RATBAG_ERROR_VALUE;

		/* attach accumulated delay to the previous action */
		if (ai > 0)
			mac.actions[ai - 1].delay = htobe16(pending_delay);
		pending_delay = 0;

		if (!pulsar_encode_action(device, ev,
					  &mac.actions[ai].code,
					  &mac.actions[ai].value))
			return RATBAG_ERROR_VALUE;

		ai++;
	}

	/* attach trailing delay to the last action */
	if (ai > 0 && pending_delay > 0)
		mac.actions[ai - 1].delay = htobe16(pending_delay);

	mac.num_actions = ai;

	/* checksum covers action section only: num_actions through last action */
	const size_t action_start = offsetof(struct pulsar_macro, num_actions);
	const size_t action_len = 1 + ai * sizeof(struct pulsar_macro_action);
	uint8_t *checksum_ptr = (uint8_t *)&mac + action_start + action_len;
	*checksum_ptr = pulsar_checksum(
		(uint8_t *)&mac + action_start, action_len);

	/* use button_index as macro slot */
	const uint16_t addr = PULSAR_MACRO_BASE_ADDR +
			button_index * sizeof(mac);

	// TODO: macro name ("ratbag <button>")

	const int rc = pulsar_write_memory_area(device, addr,
		(uint8_t *)&mac, sizeof(mac));
	if (rc < 0)
		return rc;

	uint8_t repeat_param;
	switch (macro->repeat_mode) {
	case RATBAG_MACRO_REPEAT_ONCE:
		repeat_param = 0x01;
		break;
	case RATBAG_MACRO_REPEAT_COUNT:
		if (macro->repeat_count == 0)
			return RATBAG_ERROR_VALUE;
		if (macro->repeat_count > 0xFD)
			repeat_param = 0xFD;
		else
			repeat_param = (uint8_t)macro->repeat_count;
		break;
	case RATBAG_MACRO_REPEAT_WHILE_HELD:
		repeat_param = 0xFE;
		break;
	case RATBAG_MACRO_REPEAT_UNTIL_BUTTON_PRESSED:
		repeat_param = 0xFF;
		break;
	default:
		return RATBAG_ERROR_VALUE;
	}

	return pulsar_write_button_assignment(device, button_index,
					      PULSAR_BTN_MODE_MACRO,
					      button_index, repeat_param);
}

static int
pulsar_commit_button(struct ratbag_device *device,
	struct ratbag_profile *profile,
	const struct ratbag_button *button)
{
	if (!button->dirty)
		return 0;

	const unsigned int bi = button->index;

	switch (button->action.type) {
	case RATBAG_BUTTON_ACTION_TYPE_NONE:
		return pulsar_write_button_assignment(device, bi,
			PULSAR_BTN_MODE_DISABLED, 0x00, 0x00);

	case RATBAG_BUTTON_ACTION_TYPE_BUTTON: {
		const uint8_t param = 1 << (button->action.action.button - 1);
		return pulsar_write_button_assignment(device, bi,
			PULSAR_BTN_MODE_MOUSE, param, 0x00);
	}

	case RATBAG_BUTTON_ACTION_TYPE_SPECIAL:
		switch (button->action.action.special) {
		case RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_CYCLE_UP:
			return pulsar_write_button_assignment(device, bi,
				PULSAR_BTN_MODE_DPI, PULSAR_DPI_CYCLE, 0x00);
		case RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_UP:
			return pulsar_write_button_assignment(device, bi,
				PULSAR_BTN_MODE_DPI, PULSAR_DPI_UP, 0x00);
		case RATBAG_BUTTON_ACTION_SPECIAL_RESOLUTION_DOWN:
			return pulsar_write_button_assignment(device, bi,
				PULSAR_BTN_MODE_DPI, PULSAR_DPI_DOWN, 0x00);
		case RATBAG_BUTTON_ACTION_SPECIAL_PROFILE_CYCLE_UP:
			return pulsar_write_button_assignment(device, bi,
				PULSAR_BTN_MODE_PROFILE, 0x00, 0x00);
		default:
			return RATBAG_ERROR_VALUE;
		}

	case RATBAG_BUTTON_ACTION_TYPE_DPI_LOCK: {
		const uint8_t param = button->action.action.dpi_lock.x / 50 - 1;
		return pulsar_write_button_assignment(device, bi,
			PULSAR_BTN_MODE_DPI_LOCK, param, 0x00);
	}

	case RATBAG_BUTTON_ACTION_TYPE_MACRO: {
		const struct ratbag_macro *macro = button->action.macro;

		if (pulsar_macro_is_combo(device, macro))
			return pulsar_commit_btn_combo(device, bi, macro);

		return pulsar_commit_btn_macro(device, bi, macro);
	}

	default:
		return RATBAG_ERROR_VALUE;
	}
}

static int
pulsar_commit_profile(struct ratbag_device *device,
	struct ratbag_profile *profile,
	int *current_profile)
{
	if (!profile->dirty)
		return 0;

	/* switch to the target profile if needed */
	if (profile->index != (unsigned)*current_profile) {
		const int rc = pulsar_write_active_profile(device, profile->index);
		if (rc < 0)
			return rc;
		*current_profile = profile->index;
	}

	/* polling rate */
	if (profile->rate_dirty) {
		const uint8_t rate_val = pulsar_hz_to_polling_rate(profile->hz);
		const int rc = pulsar_write_setting_byte(device,
			PULSAR_ADDR_POLLING_RATE, rate_val);
		if (rc < 0)
			return rc;
	}

	/* debounce */
	if (profile->debounce_dirty) {
		const int rc = pulsar_write_setting_byte(device,
		PULSAR_ADDR_DEBOUNCE, profile->debounce);
		if (rc < 0)
			return rc;
	}

	/* lift off distance */
	if (profile->lod_dirty) {
		const int rc = pulsar_write_setting_byte(device,
			PULSAR_ADDR_LOD, (uint8_t)profile->lod);
		if (rc < 0)
			return rc;
	}

	/* angle snapping */
	if (profile->angle_snapping_dirty) {
		const int rc = pulsar_write_setting_byte(device,
			PULSAR_ADDR_ANGLE_SNAPPING,
			profile->angle_snapping ? 0x01 : 0x00);
		if (rc < 0)
			return rc;
	}

	/* motion sync */
	if (profile->motion_sync_dirty) {
		const int rc = pulsar_write_setting_byte(device,
			PULSAR_ADDR_MOTION_SYNC,
			profile->motion_sync ? 0x01 : 0x00);
		if (rc < 0)
			return rc;
	}

	/* ripple control */
	if (profile->ripple_control_dirty) {
		const int rc = pulsar_write_setting_byte(device,
			PULSAR_ADDR_RIPPLE_CONTROL,
			profile->ripple_control ? 0x01 : 0x00);
		if (rc < 0)
			return rc;
	}

	/* autosleep */
	if (profile->autosleep_dirty) {
		const int rc = pulsar_write_setting_byte(device,
			PULSAR_ADDR_AUTOSLEEP,
			profile->autosleep / 10);
		if (rc < 0)
			return rc;
	}

	/* DPI resolutions */
	unsigned int dpi_count = 0;
	struct ratbag_resolution *resolution;
	ratbag_profile_for_each_resolution(profile, resolution) {
		if (!resolution->is_disabled)
			dpi_count = resolution->index + 1;

		if (!resolution->dirty)
			continue;

		const int rc = pulsar_write_dpi(device, resolution->index,
			resolution->dpi_x);
		if (rc < 0)
			return rc;
	}

	/* update dpi mode count */
	struct pulsar_data *drv_data = profile->drv_data;
	uint8_t prev_dpi_count =
		drv_data->memory[profile->index].settings[PULSAR_ADDR_DPI_MODE_COUNT];
	if (dpi_count != prev_dpi_count) {
		const int rc = pulsar_write_setting_byte(device,
		PULSAR_ADDR_DPI_MODE_COUNT, dpi_count);
		if (rc < 0)
			return rc;
	}

	/* LEDs */
	struct ratbag_led *led;
	ratbag_profile_for_each_led(profile, led) {
		if (!led->dirty)
			continue;

		int rc = 0;

		if (led->index == 0) {
			/* LED 0: global effect settings */
			rc = pulsar_write_led(device, led);
		} else {
			/* LEDs 1-8: per-DPI-mode color */
			rc = pulsar_write_dpi_color(device,
			led->index - 1, led->color);
		}
		if (rc < 0)
			return rc;
	}

	/* Buttons */
	struct ratbag_button *button;
	ratbag_profile_for_each_button(profile, button) {
		const int rc = pulsar_commit_button(device, profile, button);
		if (rc < 0)
			return rc;
	}

	return 0;
}

static int
pulsar_commit(struct ratbag_device *device)
{
	struct ratbag_profile *profile;

	ratbag_device_for_each_profile(device, profile) {
		const int rc = pulsar_check_profile(device, profile);
		if (rc < 0)
			return rc;
	}

	/* status check before writing */
	int rc = pulsar_read_status(device);
	if (rc < 0)
		return rc;
	if (rc == 0) {
		log_error(device->ratbag,
			"%s: device not responding (mouse asleep?)\n", __func__);
		return -EAGAIN;
	}

	int original_profile = pulsar_read_active_profile(device);
	if (original_profile < 0)
		return original_profile;

	int current_profile = original_profile;

	ratbag_device_for_each_profile(device, profile) {
		rc = pulsar_commit_profile(device, profile, &current_profile);
		if (rc < 0)
			break;
	}

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

static int
pulsar_set_active_profile(struct ratbag_device *device, unsigned int index)
{
	if (index >= PULSAR_NUM_PROFILES)
		return -EINVAL;

	const int rc = pulsar_write_active_profile(device, index);
	if (rc < 0)
		return rc;

	struct pulsar_data *drv_data = ratbag_get_drv_data(device);
	drv_data->active_profile = index;

	return 0;
}

static void
pulsar_handle_dpi_event(struct ratbag_device *device)
{
	int dpi_mode = pulsar_read_active_dpi_mode(device);
	if (dpi_mode < 0) {
		log_error(device->ratbag,
			"%s: failed to read dpi mode from device (%d)\n",
			__func__, dpi_mode);
		return;
	}

	struct pulsar_data *drv_data = device->drv_data;
	const uint8_t p = drv_data->active_profile;
	const int prev_dpi =
		drv_data->memory[p].settings[PULSAR_ADDR_ACTIVE_DPI_MODE];

	if (dpi_mode == prev_dpi)
		return;

	struct ratbag_profile *profile = ratbag_device_get_profile(device, p);
	ratbag_profile_get_resolution(profile, prev_dpi)->is_active = false;
	ratbag_profile_get_resolution(profile, dpi_mode)->is_active = true;
	drv_data->memory[p].settings[PULSAR_ADDR_ACTIVE_DPI_MODE] = dpi_mode;

	log_info(device->ratbag,
		"%s: dpi_mode %d\n", __func__, dpi_mode);
}

static void
pulsar_handle_profile_event(struct ratbag_device *device)
{
	int active_profile = pulsar_read_active_profile(device);
	if (active_profile < 0) {
		log_error(device->ratbag,
			"%s: failed to read active profile from device (%d)\n",
			__func__, active_profile);
		return;
	}

	struct pulsar_data *drv_data = device->drv_data;
	drv_data->active_profile = active_profile;
	log_info(device->ratbag,
		"%s: active profile %d\n", __func__, active_profile);
}

static unsigned int
pulsar_handle_event(struct ratbag_device *device,
	const uint8_t* buf, size_t len, int hidraw_index)
{
	log_raw(device->ratbag,
		"%s: hidraw_index=%d len=%zu\n", __func__, hidraw_index, len);
	log_buf_raw(device->ratbag, "pulsar event data: ", buf, len);

	if (len != PULSAR_PACKET_SIZE) {
		log_debug(device->ratbag,
			"%s: discard event with invalid length %zu\n",
			__func__, len);
		return RATBAG_EVENT_NONE;
	}

	const struct pulsar_payload *payload =
		(const struct pulsar_payload *) buf;

	if (payload->cmd != PULSAR_CMD_DEVICE_EVENT) {
		/* ignore; can be caused by a kernel battery driver */
		return RATBAG_EVENT_NONE;
	}

	switch (payload->data[4]) {
	case PULSAR_EVENT_DPI:
		log_debug(device->ratbag, "%s: received DPI event\n", __func__);
		pulsar_handle_dpi_event(device);
		return RATBAG_EVENT_RESOLUTION_CHANGED;
	case PULSAR_EVENT_PROFILE:
		log_debug(device->ratbag, "%s: received profile event\n", __func__);
		pulsar_handle_profile_event(device);
		return RATBAG_EVENT_RESOLUTION_CHANGED;
	case PULSAR_EVENT_POWER:
		log_debug(device->ratbag, "%s: received power event\n", __func__);
		return RATBAG_EVENT_BATTERY_CHANGED;
	default:
		log_bug_libratbag(device->ratbag, "%s: unknown event %02x\n",
			__func__, (int) payload->data[4]);
	}

	return RATBAG_EVENT_NONE;
}

static int
pulsar_reset (struct ratbag_device* device) {
	struct pulsar_payload request = {.data = {0}};
	struct pulsar_payload response;

	log_info(device->ratbag, "%s: reset request\n", __func__);

	pulsar_finalize_payload(&request, PULSAR_CMD_RESTORE);
	int rc = pulsar_send_command(device, &request);
	if (rc < 0)
		return rc;

	/* Reset can take a few seconds; retry the read since the default
	 * hidraw timeout is 1s. */
	for (int i = 0; i < 3; ++i) {
		rc = pulsar_read_response(device, &response);
		if (rc == -ETIMEDOUT) {
			log_debug(device->ratbag, "%s: ETIMEDOUT", __func__);
			continue;
		}
		if (rc < 0)
			return rc;
		break;
	}

	if (rc < 0)
		return rc;

	/* Re-read profile data from the now-reset device. */
	struct pulsar_data *drv_data = device->drv_data;
	const int active_profile = pulsar_read_active_profile(device);
	if (active_profile < 0)
		return active_profile;

	drv_data->active_profile = active_profile;

	struct ratbag_profile *profile;
	ratbag_device_for_each_profile(device, profile) {
		profile->is_active = (profile->index == (unsigned)active_profile);
		profile->loaded = false;
		if (profile->is_active) {
			rc = pulsar_read_profile_settings(profile);
			if (rc < 0)
				return rc;
			profile->loaded = true;
		}
	}

	return 0;
}

/* ----- ratbag driver struct ----- */

struct ratbag_driver pulsar_driver = {
	.name = "Pulsar",
	.id = "pulsar",
	.probe = pulsar_probe,
	.remove = pulsar_remove,
	.commit = pulsar_commit,
	.read_profile = pulsar_read_profile,
	.set_active_profile = pulsar_set_active_profile,
	.handle_event = pulsar_handle_event,
	.reset = pulsar_reset,
};
