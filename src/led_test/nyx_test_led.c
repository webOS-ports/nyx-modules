// Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

/*
 * nyx-test-led - drive a NYX_DEVICE_LED instance, i.e. the torch.
 *
 * Companion to nyx-test-ledcontroller, which drives the other LED device type.
 * They are kept apart because they are genuinely different nyx devices: the
 * controller does effects on the notification LEDs, this does plain brightness
 * on a single named LED.
 */

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nyx/nyx_client.h>

static void usage(const char *prog)
{
	printf(
	    "Usage: %s [-d <instance>] <on|off|get|set <percent>>\n"
	    "\n"
	    "Commands:\n"
	    "  on             full brightness (100%%)\n"
	    "  off            0%%\n"
	    "  set <percent>  0-100\n"
	    "  get            report current brightness\n"
	    "\n"
	    "Options:\n"
	    "  -d <instance>  NYX_DEVICE_LED instance name (default: Torch, which\n"
	    "                 nyx resolves to nyxLedTorch.module)\n"
	    "  -t <seconds>   hold the torch on for <seconds>, then switch it off.\n"
	    "                 Needed to see anything: closing the nyx device switches\n"
	    "                 the torch off, so a plain \"on\" lights it only for as long\n"
	    "                 as this process takes to exit. A service keeps the handle\n"
	    "                 open instead; this is the equivalent for a one-shot CLI.\n"
	    "\n"
	    "Examples:\n"
	    "  %s on\n"
	    "  %s set 30\n"
	    "  %s get\n"
	    "  %s off\n", prog, prog, prog, prog, prog);
}

int main(int argc, char **argv)
{
	const char *prog = (argc > 0 && argv[0]) ? basename(argv[0]) : "nyx-test-led";
	const char *instance = "Torch";
	int hold_seconds = 0;
	nyx_device_handle_t device = NULL;
	nyx_error_t err;
	int32_t brightness;
	int argi = 1;

	while (argc > argi + 1 &&
	        (strcmp(argv[argi], "-d") == 0 || strcmp(argv[argi], "-t") == 0))
	{
		if (strcmp(argv[argi], "-d") == 0)
		{
			instance = argv[argi + 1];
		}
		else
		{
			hold_seconds = atoi(argv[argi + 1]);
		}

		argi += 2;
	}

	if (argi >= argc)
	{
		usage(prog);
		return 1;
	}

	err = nyx_device_open(NYX_DEVICE_LED, instance, &device);

	if (err != NYX_ERROR_NONE || device == NULL)
	{
		fprintf(stderr,
		        "error: could not open NYX_DEVICE_LED \"%s\" (nyx error %d).\n"
		        "Either nyxLed%s.module is not installed, or it found no torch node:\n"
		        "check /sys/class/leds for one whose name contains \"torch\", and\n"
		        "[module.led] torch_path in /etc/nyx.conf.\n", instance, err, instance);
		return 1;
	}

	if (strcmp(argv[argi], "get") == 0)
	{
		err = nyx_led_get_brightness(device, &brightness);

		if (err != NYX_ERROR_NONE)
		{
			fprintf(stderr, "error: could not read brightness: nyx error %d\n", err);
			nyx_device_close(device);
			return 1;
		}

		printf("%s: %d%%\n", instance, brightness);
		nyx_device_close(device);
		return 0;
	}

	if (strcmp(argv[argi], "on") == 0)
	{
		brightness = 100;
	}
	else if (strcmp(argv[argi], "off") == 0)
	{
		brightness = 0;
	}
	else if (strcmp(argv[argi], "set") == 0)
	{
		if (argi + 1 >= argc)
		{
			fprintf(stderr, "error: 'set' needs a percentage\n");
			nyx_device_close(device);
			return 1;
		}

		brightness = atoi(argv[argi + 1]);
	}
	else
	{
		fprintf(stderr, "error: unknown command '%s'\n", argv[argi]);
		usage(prog);
		nyx_device_close(device);
		return 1;
	}

	printf("%s: setting %d%%\n", instance, brightness);
	fflush(stdout);

	err = nyx_led_set_brightness(device, brightness);

	if (err == NYX_ERROR_INVALID_VALUE)
	{
		fprintf(stderr, "error: brightness must be 0-100\n");
		nyx_device_close(device);
		return 1;
	}

	if (err != NYX_ERROR_NONE)
	{
		fprintf(stderr,
		        "error: setting brightness failed: nyx error %d.\n"
		        "If the node exists, this is usually permissions - the sysfs write is\n"
		        "only reported at close. See the udev rules that relax the torch node.\n",
		        err);
		nyx_device_close(device);
		return 1;
	}

	printf("ok\n");

	if (hold_seconds > 0 && brightness > 0)
	{
		printf("holding for %ds, then switching off...\n", hold_seconds);
		fflush(stdout);
		sleep((unsigned)hold_seconds);
		nyx_led_set_brightness(device, 0);
		printf("off\n");
	}

	nyx_device_close(device);
	return 0;
}
