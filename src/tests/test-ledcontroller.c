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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>


/*! 
 * typdefs for user command-line option help text
 * and help text to use for command line args
 */
typedef struct __Command_Line_Options__
{
	char option ;
	char *help_text ;
} Option_help ;


Option_help help_strings[] =
{
	{ 'h' , "-h,--help...........- Print this help menu" },
	{ 'd' , "-d,--device.........- required_argument - name of led device [backlight,centre]" },
	{ 'e' , "-e,--effect.........- required_argument - [ set_brightness,pulsate]" },
	{ 'p' , "-p,--param..........- params for each led; comma separated params in format var=value\n" 
			"                      ..........  i.e param1=value1,param2=value2,param3=value3.\n"
			"                      ..........  The required params are specified below:\n"
			"                      ..........  1) set_brightness : brightness(level 0 - 255)\n"
			"                      ..........  2) pulsate : fade_on(in milli-sec),fade_off(in milli-sec)\n" },
	{ '0' , NULL }
};

/* Columns are   1) Long-option-name  2) required/optional/no argument  3) store in pointer 4) short-option */
const struct option g_program_options[] =
{
    {  "help"     , no_argument          ,   NULL  , 'h' }   ,
    {  "device"   , required_argument    ,   NULL  , 'd' }   ,
    {  "effect"   , required_argument    ,   NULL  , 'e' }   ,
    {  "param"    , required_argument    ,   NULL  , 'p' }   ,
    {  NULL       , no_argument          ,   NULL  ,  0  }
};


/*! 
 * Function Declarations for internal functions 
 */
static void usage( char *);
static char * generate_option_short_string(const struct option *);
static int digest_options( int , char **argv , 
                           const struct option *, nyx_led_controller_effect_t *);
static int map_led_device(char *, nyx_led_controller_effect_t *);
static int map_led_effect(char *, nyx_led_controller_effect_t *);
static int map_led_effect_param(char *, nyx_led_controller_effect_t *);


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
	int err = 0 ;
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

	err = digest_options(argc , argv , g_program_options , &effect);
	if(err)
	{
		if( err != -2 )
			g_error("Error parsing cmdline options");
		goto err_main ;
	}

	error = nyx_led_controller_execute_effect(leddevice, effect);

err_main :
	/* Closing down test */
	if( effect.core_configuration != NULL )
	{
		error = nyx_led_controller_core_configuration_release( effect.core_configuration );
		if(error != NYX_ERROR_NONE)
			g_error("Could not free core_configuration structure created for tests.error=[%d]", error);
	}

	g_message("Closing LED controller device ...");
	error = nyx_device_close(leddevice);
	if (error != NYX_ERROR_NONE) {
		g_warning("Unable to release LED controller device");
		exit(1);
	}

	nyx_deinit();

	return 0 ;
}

/*! 
 * \brief     - provides user with help banner either on 
 *              cmd-line error or when called with -h
 *
 * \param[in] - prog - name of current executable 
 * \return    - void - nothing to return 
 */
static void usage( char *prog )
{
	unsigned int idx = 0 ;

	g_message("==============================================");
	g_message("Program Usage for : %s" , (prog != NULL)? prog : "test_led");
	g_message("Note : order of params is important. Please specify in order  device,effect,params");
	g_message("==============================================");

	for( idx=0 ; help_strings[idx].help_text != NULL ; idx++ )
		g_message("%s", help_strings[idx].help_text);
}

/*! 
 * \brief     - parse list of options and generate option-string
 *              for getopt_long()
 *
 * \param[in] - prog_opts - list of options available
 * \return    - on success - a pointer holding the short-option-list
 *              on failure - NULL
 */
char * generate_option_short_string(const struct option *prog_opts)
{
	int idx_i , idx_j , incr = 0 ;
	int optstr_len = 0 ;
	char *shortopt_list = NULL ;
	char buffer[3];

	/* Sanity check inputs */
	if( prog_opts == NULL )
	{
		g_error("No valid program options are available");
		goto err_supported_option_get ;
	}

	/* calculate length of short option list */
	for( idx_i=0 ; prog_opts[idx_i].name != NULL ; idx_i++ )
	{
		optstr_len += sizeof(char);
		switch( prog_opts[idx_i].has_arg ) 
		{
			case required_argument :
				optstr_len += strlen(":");
				break ;
			case optional_argument :
				optstr_len += strlen("::");
				break ;
			case no_argument :
				break ;
			default : 
				g_error("options command-line arguments not specified correctly");
				goto err_supported_option_get ;
		}
	}

	/* malloc() enough space to hold string */
	shortopt_list = malloc(optstr_len + 1);
	if( shortopt_list == NULL )
	{
		g_error("Cannot alloc(%u) bytes for short-option list", optstr_len + 1);
		goto err_supported_option_get ;
	}
	memset(shortopt_list , 0 , optstr_len + 1);

	/* generate short-option list */
	for( idx_i=0 , idx_j=0 ; prog_opts[idx_i].name != NULL ; idx_i++ , idx_j += (incr+1))
	{
		memset( buffer , 0 , sizeof(buffer));
		switch( prog_opts[idx_i].has_arg ) 
		{
			case required_argument :
				strncpy( buffer , ":" , sizeof(buffer));
				break ;
			case optional_argument :
				strncpy( buffer , "::" , sizeof(buffer));
				break ;
			case no_argument :
				break ;
			default : 
				g_error("invalid cmdline option param characteristic(%d) specified", prog_opts[idx_i].has_arg );
				goto err_supported_option_get ;
		}
		incr = strlen(buffer);
		snprintf(&(shortopt_list[idx_j]), optstr_len+1 - idx_j ,"%c%s", prog_opts[idx_i].val , buffer );
	}
	
	return shortopt_list ;

err_supported_option_get : 
	shortopt_list = (shortopt_list != NULL) ? free(shortopt_list),NULL : NULL ;
	return shortopt_list ;
}

/*! 
 * \brief         - digest all options passed into the program 
 * \param[in]     - int argc - number of arguments as passed into main
 * \param[in]     - char **argv - list of arguments as passed into main
 * \param[in]     - struct option *prog_opts - list of various cmd-line args that 
 *                  can be accepted and parsed
 * \param[out]    - nyx_led_controller_effect_t *effect - ptr to led effect requested 
 * \return        - on success 0 , otherwise -1. if -h option is used or there is 
 *                  illegal cmdline structure, -2 is returned
 */
static int digest_options( int argc , char **argv 
							, const struct option *prog_opts 
							, nyx_led_controller_effect_t *effect )
{
	int err = 0 , opt = 0 ;
	char *short_opts = NULL ;
	

	/* Sanity Check input params */
	if((argv == NULL ) || (*argv == NULL)) 
	{
		g_error("No cmdline params found to digest");
		err = -1 ;
		goto err_option_digest ;
	}
	if( effect == NULL ) 
	{
		g_error("cmd-line option digest expects effect structure being passed in"); 
		err = -1 ;
		goto err_option_digest ;
	}
	if( prog_opts == NULL )
	{
		g_error("cmd-line option digest expects effect structure being passed in"); 
		err = -1 ;
		goto err_option_digest ;
	}

	/* init the structure */
	memset( effect , 0 , sizeof(nyx_led_controller_effect_t));

	/* generate short-option strings */
	short_opts = generate_option_short_string( prog_opts );
	if( short_opts == NULL )
	{
		g_error("Error generating short-option list for cmd-line params");
		err = -1 ;
		goto err_option_digest ;
	}

	optind = 1 ;
	for( opt = getopt_long(argc, argv, short_opts, prog_opts , NULL) ;
	     opt != -1 ;
	     opt = getopt_long(argc, argv, short_opts, prog_opts , NULL)) 
	{
		switch( opt ) 
		{
			case '?' : /* intentional fall through */
			case ':' : /* intentional fall through */
				g_error("getopt() : Error parsing arguments");
			case 'h' :
				usage(*argv);
				err = -2 ;
				goto err_option_digest ;
			case 'd' : 
				err = map_led_device(optarg , effect);
				if(err)
					g_error("Check for valid LED device[%s] failed. err = %d",optarg , err);

				break ;
			case 'e' : 
				err = map_led_effect(optarg , effect);
				if(err)
					g_error("Check for valid LED device effect[%s] failed. err = %d",optarg , err);

				break ;
			case 'p' : 
				err = map_led_effect_param(optarg , effect);
				if(err)
					g_error("Check for valid LED params[%s] failed. err = %d",optarg , err);

				break ;
		}
	}

	if( argc < 2 )
	{
		usage(*argv);
		err = -2 ;
		goto err_option_digest ;
	}

	short_opts = (short_opts != NULL) ? free(short_opts), NULL : NULL ;
	return err ;

err_option_digest :
	short_opts = (short_opts != NULL) ? free(short_opts), NULL : NULL ;
	return err ;
}

/*! 
 * \brief         - Check which LED device is being activated
 * \param[in]     - led - nyx LED device name 
 * \param[in/out] - nyx_led_controller_effect_t *effect - ptr to led effect requested 
 * \return        - on success 0 , otherwise -1 
 */
static int map_led_device(char *led , nyx_led_controller_effect_t *effect)
{
	int err = 0 ;

	/* Sanity check input params */
	if( led == NULL ) 
	{
		g_error("No LED cmd-line param to check : led = NULL");
		err = -1 ;
		goto err_led_dev_resolve ;
	}
	if( effect == NULL )
	{
		g_error("No LED params to map into : effect = NULL");
		err = -1 ;
		goto err_led_dev_resolve ;
	}

	if( strcmp(led , "backlight") == 0 ) 
	{
		effect->required.led = NYX_LED_CONTROLLER_BACKLIGHT_LEDS ;
		effect->backlight.callback = led_controller_cb;
		effect->backlight.callback_context = NULL;
	}
	else if( strcmp(led , "centre") == 0 )
	{
		effect->required.led = NYX_LED_CONTROLLER_CENTER_LED ;
	}
	else 
	{
		err = -1 ;
	}

err_led_dev_resolve :
	return err ;

}



/*! 
 * \brief         - map nyx LED effect 
 * \param[in]     - nyx LED device effect param from cmd-line 
 * \param[in/out] - nyx_led_controller_effect_t *effect - ptr to led effect requested 
 * \return        - on success 0 , otherwise -1 
 */
static int map_led_effect(char *param , nyx_led_controller_effect_t *effect)
{
	int err = 0 ;

	/* Sanity check input params */
	if( param == NULL ) 
	{
		g_error("No LED cmd-line param to check : led = NULL");
		err = -1 ;
		goto err_led_effect_map ;
	}
	if( effect == NULL )
	{
		g_error("No LED params to map into : effect = NULL");
		err = -1 ;
		goto err_led_effect_map ;
	}

	if( strcmp(param , "set_brightness") == 0 ) 
	{
		effect->required.effect = NYX_LED_CONTROLLER_EFFECT_LED_SET ;
	}
	else if( strcmp(param , "pulsate") == 0 )
	{
		effect->required.effect = NYX_LED_CONTROLLER_EFFECT_LED_PULSATE ;
	}
	else 
	{
		err = -1 ;
	}


	/*!
	 * Sanity check that only the correct devices have the corresponding
	 * effects set-up.As more effects are added, we may need to rejig this 
	 * list further
	 */
	switch( effect->required.led )
	{
		case NYX_LED_CONTROLLER_KEYPAD : 
		case NYX_LED_CONTROLLER_LCD    : 
		case NYX_LED_CONTROLLER_BACKLIGHT_LEDS :
			switch( effect->required.effect )
			{
				case NYX_LED_CONTROLLER_EFFECT_LED_SET :
					break ;
				default : 
					g_error("currently only set_brightness allowed for backlight");
					err = -1 ;
					goto err_led_effect_map ;
			}
			break ;
		case NYX_LED_CONTROLLER_CENTER_LED :
			switch( effect->required.effect )
			{
				case NYX_LED_CONTROLLER_EFFECT_LED_SET :
				case NYX_LED_CONTROLLER_EFFECT_LED_PULSATE :
					break ;
				default : 
					g_error("currently only set_brightness,pulsate allowed for centre_led");
					err = -1 ;
					goto err_led_effect_map ;
			}
			break ;
		default : 
			break ;    /* nothing to do */
	}

	return err ;

err_led_effect_map :
	effect->required.effect = NYX_LED_CONTROLLER_EFFECT_UNDEFINED ;  /* reset for error */
	return err ;
}

/*! 
 * \brief         - Populate all params required for LED effects
 * \param[in]     - param_list - comma-separated list of params in [name=value] format
 * \param[in/out] - nyx_led_controller_effect_t *effect - ptr to led effect requested 
 * \return        - on success 0 , otherwise -1 
 */
static int map_led_effect_param(char *effect_list , nyx_led_controller_effect_t *effect)
{
	int err = 0 ;
	nyx_error_t nyx_err = NYX_ERROR_NONE ;
	nyx_led_controller_parameter_type_t led_param ;

	char *list = NULL , *param = NULL , *safe = NULL ;
	char *ch = NULL , *var = NULL , *value = NULL ;

	/* Sanity check input params */
	if( effect_list == NULL ) 
	{
		g_error("No LED cmd-line param to check : effect_list = NULL");
		err = -1 ;
		goto err_led_effect_resolve ;
	}
	if( effect == NULL )
	{
		g_error("No LED effect param to map into : effect = NULL");
		err = -1 ;
		goto err_led_effect_resolve ;
	}
	
	if((effect->required.led <= NYX_LED_CONTROLLER_NONE_LED )
	   || (effect->required.led > NYX_LED_CONTROLLER_ALL_LEDS )) 
	{
		g_error("LED device not-specified/specified-order-wrong or wrong one specified");
		err = -1 ;
		goto err_led_effect_resolve ;
	}


	nyx_err = nyx_led_controller_core_configuration_create( effect->required.led 
	                                                        , &(effect->core_configuration));
	if( nyx_err != NYX_ERROR_NONE )
	{
		g_error("Error Creating LED effect configuration. nyx-err = %d " , nyx_err );
		err = -1 ;
		goto err_led_effect_resolve ;
	}

	list = strdup( effect_list );
	if( list == NULL )
	{
		g_error("Could not duplicate led effect param argument");
		err = -1 ;
		goto err_led_effect_resolve ;
	}


	for( param = strtok_r(list,",",&safe) ; param != NULL ; param = strtok_r(NULL,",",&safe))
	{
		ch = strchr( param , '=');
		if( ch == NULL )
		{
			g_error("effect param [%s] in wrong format. Should be in format [var=value]",param);
			err = -1 ;
			goto err_led_effect_resolve ;
		}

		var = strndup( param , (size_t)(ch - param));
		value = strndup(ch + 1 , (size_t)strlen(ch));

		if( strcmp(var , "brightness") == 0 ) 
		{
			switch(effect->required.led)
			{
				case NYX_LED_CONTROLLER_KEYPAD         :  
					effect->backlight.brightness_keypad = strtol(value,NULL,0);
					break ;
				case NYX_LED_CONTROLLER_LCD            :  
					effect->backlight.brightness_lcd = strtol(value,NULL,0);
					break ;
				case NYX_LED_CONTROLLER_BACKLIGHT_LEDS : 
					effect->backlight.brightness_keypad = strtol(value,NULL,0);
					effect->backlight.brightness_lcd = strtol(value,NULL,0);
					break ;
				case NYX_LED_CONTROLLER_ALL_LEDS       : 
					effect->backlight.brightness_keypad = strtol(value,NULL,0);
					effect->backlight.brightness_lcd = strtol(value,NULL,0);
					/* intentional fall through to default to set up LED structure */
				default :
					nyx_err = nyx_led_controller_core_configuration_set_param(effect->core_configuration 
			   	                                                              , NYX_LED_CONTROLLER_CORE_EFFECT_BRIGHTNESS
			   	                                                              , strtol(value,NULL,0));
					if(nyx_err != NYX_ERROR_NONE)
					{
						g_error("Error setting LED config param.err = %d", nyx_err);
						err = -1 ;
						goto err_loop ;
					}
		
			}
		} 
		else    /* not brightness ie other fancy tricks */
		{
			if( strcmp(var , "fade_on") == 0 ) 
			{
				led_param = NYX_LED_CONTROLLER_CORE_EFFECT_FADE_IN;
			} 
			else if( strcmp(var , "fade_off") == 0 ) 
			{
				led_param = NYX_LED_CONTROLLER_CORE_EFFECT_FADE_OUT;
			} 
			else 
			{
				g_error("found unknown param within parameter argument [%s]\n", var );
				err = -1 ;
				goto err_loop ;
			}

			nyx_err = nyx_led_controller_core_configuration_set_param(effect->core_configuration 
			                                                          , led_param , strtol(value,NULL,0));
			if(nyx_err != NYX_ERROR_NONE)
			{
				g_error("Error setting LED config param[%s] to value[%s].err = %d", var , value ,nyx_err);
				err = -1 ;
				goto err_loop ;
			}
		} 

		value = (value != NULL) ? free(value) , NULL : NULL ;
		var = (var != NULL) ? free(var) , NULL : NULL ;
	}


	list = (list != NULL) ? free(list) , NULL : NULL ;
	return err ;

err_loop :
	value = (value != NULL) ? free(value) , NULL : NULL ;
	var = (var != NULL) ? free(var) , NULL : NULL ;

err_led_effect_resolve :
	list = (list != NULL) ? free(list) , NULL : NULL ;
	if( effect->core_configuration ) 
	{
		nyx_err = nyx_led_controller_core_configuration_release( effect->core_configuration );
		if( nyx_err != NYX_ERROR_NONE )
			g_error("Could not release nyx-effect-configuration params. err = %d", nyx_err);
	}
	return err ;
}
