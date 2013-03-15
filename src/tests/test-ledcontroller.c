/* @@@LICENSE
*
*      Copyright (c) 2010-2012 Hewlett-Packard Development Company, L.P.
*      Copyright (c) 2012 Simon Busch <morphis@gravedo.de>
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <glib.h>
#include <nyx/nyx_client.h>

void led_controller_cb(nyx_device_handle_t device,
		nyx_callback_status_t status, void *context)
{
	if (status != NYX_CALLBACK_STATUS_DONE) {
		g_warning("Setting backlight brightness through LED controller failed!");
		return;
	}

	g_message("Setting backlight brightness through LED controller was successfull");
}

int main(int argc, char **argv)
{
	nyx_device_handle_t *leddevice;
	nyx_error_t error;
	nyx_led_controller_effect_t effect;

	nyx_init();

	g_message("Opening LED controller device ...");
	error = nyx_device_open(NYX_DEVICE_LED_CONTROLLER, "Default", &leddevice);

	if ((error != NYX_ERROR_NONE) || (leddevice == NULL)) {
		g_warning("Failed to open LED controller device");
		exit(1);
	}

	effect.required.effect = NYX_LED_CONTROLLER_EFFECT_LED_SET;
	effect.backlight.callback = led_controller_cb;
	effect.backlight.callback_context = NULL;
	effect.required.led = NYX_LED_CONTROLLER_BACKLIGHT_LEDS;
	effect.backlight.brightness_lcd = 0;

	error = nyx_led_controller_execute_effect(leddevice, effect);

	g_message("Closing LED controller device ...");
	error = nyx_device_close(leddevice);
	if (error != NYX_ERROR_NONE) {
		g_warning("Unable to release LED controller device");
		exit(1);
	}

	nyx_deinit();

	return 0;
}
