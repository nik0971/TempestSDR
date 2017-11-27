/*
#-------------------------------------------------------------------------------
# Copyright (c) 2014 Martin Marinov.
# All rights reserved. This program and the accompanying materials
# are made available under the terms of the GNU Public License v3.0
# which accompanies this distribution, and is available at
# http://www.gnu.org/licenses/gpl.html
# 
# Contributors:
#     Martin Marinov - initial API and implementation
#     Henning Paul - port to librtlsdr
#-------------------------------------------------------------------------------
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boost/program_options.hpp>
#include <boost/format.hpp>
#include <boost/thread.hpp>

#include <iostream>
#include <complex>

#include "TSDRPlugin.h"

#include "TSDRCodes.h"

#include <stdint.h>
#include <boost/algorithm/string.hpp>

#include "errors.hpp"

#include "rtl-sdr.h"

#define HOW_OFTEN_TO_CALL_CALLBACK_SEC (0.06)
#define FRACT_DROPPED_TO_TOLERATE (0)

namespace po = boost::program_options;

uint32_t req_freq = 105e6;
int req_gain = 0;
uint32_t req_rate = 2800000;
volatile int is_running = 0;
rtlsdr_dev_t *dev = NULL;

EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_getName(char * name) {
	strcpy(name, "TSDR RTL-SDR Compatible Plugin");
}

int tortlsdrgain(float gain) {
	return (int)(gain*10);
}

int nearest_gain(rtlsdr_dev_t *dev, int target_gain)
{
	int i, r, err1, err2, count, nearest;
	int* gains;
	r = rtlsdr_set_tuner_gain_mode(dev, 1);
	if (r < 0) {
		fprintf(stderr, "WARNING: Failed to enable manual gain.\n");
		return r;
	}
	count = rtlsdr_get_tuner_gains(dev, NULL);
	if (count <= 0) {
		return 0;
	}
	gains = (int *)malloc(sizeof(int) * count);
	count = rtlsdr_get_tuner_gains(dev, gains);
	nearest = gains[0];
	for (i=0; i<count; i++) {
		err1 = abs(target_gain - nearest);
		err2 = abs(target_gain - gains[i]);
		if (err2 < err1) {
			nearest = gains[i];
		}
	}
	free(gains);
	return nearest;
}


int verbose_device_search(const char *s)
{
	int i, device_count, device, offset;
	char *s2;
	char vendor[256], product[256], serial[256];
	device_count = rtlsdr_get_device_count();
	if (!device_count) {
		fprintf(stderr, "No supported devices found.\n");
		return -1;
	}
	fprintf(stderr, "Found %d device(s):\n", device_count);
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		fprintf(stderr, "  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
	}
	fprintf(stderr, "\n");
	/* does string look like raw id number */
	device = (int)strtol(s, &s2, 0);
	if (s2[0] == '\0' && device >= 0 && device < device_count) {
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string exact match a serial */
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (strcmp(s, serial) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string prefix match a serial */
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		if (strncmp(s, serial, strlen(s)) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	/* does string suffix match a serial */
	for (i = 0; i < device_count; i++) {
		rtlsdr_get_device_usb_strings(i, vendor, product, serial);
		offset = strlen(serial) - strlen(s);
		if (offset < 0) {
			continue;}
		if (strncmp(s, serial+offset, strlen(s)) != 0) {
			continue;}
		device = i;
		fprintf(stderr, "Using device %d: %s\n",
			device, rtlsdr_get_device_name((uint32_t)device));
		return device;
	}
	fprintf(stderr, "No matching devices found.\n");
	return -1;
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_init(const char * params) {
	// simulate argv and argc
	std::string sparams(params);

	typedef std::vector< std::string > split_vector_type;

	split_vector_type argscounter;
	boost::split( argscounter, sparams, boost::is_any_of(" "), boost::token_compress_on );

	const int argc = argscounter.size()+1;
	char ** argv = (char **) malloc(argc*sizeof(char *));
	char zerothtarg[] = "TSDRPlugin_librtlsdr";
	argv[0] = (char *) zerothtarg;
	for (int i = 0; i < argc-1; i++)
		argv[i+1] = (char *) argscounter[i].c_str();

	//variables to be set by po
	std::string args, file;
	uint32_t bw = 0;
	int dev_index;
	int r;

	//setup the program options
	po::options_description desc("Allowed options");
	desc.add_options()
			("args", po::value<std::string>(&args)->default_value(""), "device address args")
			("rate", po::value<uint32_t>(&req_rate)->default_value(req_rate), "rate of incoming samples")
			("bw", po::value<uint32_t>(&bw), "bandwidth in Hz");

	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);
		po::notify(vm);
	} catch (std::exception const&  ex)
	{
		std::string msg(boost::str(boost::format("Error: %s\n\nTSDRPlugin_librtlsdr %s") % ex.what() % desc));
		RETURN_EXCEPTION(msg.c_str(), TSDR_PLUGIN_PARAMETERS_WRONG);
	}

	if (args.length()>0) {
		dev_index = verbose_device_search(args.c_str());
	}
	else {
		dev_index = verbose_device_search("0");
	}
	
	if (dev_index < 0) {
		RETURN_EXCEPTION("Can't find RTL-SDR.", TSDR_CANNOT_OPEN_DEVICE);
	}

	r = rtlsdr_open(&dev, (uint32_t)dev_index);
	if (r < 0) {
		RETURN_EXCEPTION("Can't open device.", TSDR_CANNOT_OPEN_DEVICE);
	}
	/* Set the sample rate */
	//verbose_set_sample_rate(dev, req_rate);
	r = rtlsdr_set_sample_rate(dev, req_rate);
	if (r < 0) {
		RETURN_EXCEPTION("Can't set sample rate.", TSDR_CANNOT_OPEN_DEVICE);
	}

	/* Set the tuner bandwidth */
	//verbose_set_bandwidth(dev, bw);
	r = rtlsdr_set_tuner_bandwidth(dev, bw);
	if (r < 0) {
		RETURN_EXCEPTION("Can't set bandwidth.", TSDR_CANNOT_OPEN_DEVICE);
	}

	/* Set the frequency */
	//verbose_set_frequency(dev, req_freq);
	r = rtlsdr_set_center_freq(dev, req_freq);
	if (r < 0) {
		RETURN_EXCEPTION("Can't set center frequency.", TSDR_CANNOT_OPEN_DEVICE);
	}

	if (0 == req_gain) {
  		/* Enable automatic gain */
		//verbose_auto_gain(dev);
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set auto gain mode.", TSDR_CANNOT_OPEN_DEVICE);
		}
	} else {
		/* Enable manual gain */
		int gain_val = nearest_gain(dev, tortlsdrgain(req_gain));
		//verbose_gain_set(dev, gain_val);
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set manual gain mode.", TSDR_CANNOT_OPEN_DEVICE);
		}
		
		r = rtlsdr_set_tuner_gain(dev, gain_val);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set gain.", TSDR_CANNOT_OPEN_DEVICE);
		}
	}
		

	free(argv);
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_setsamplerate(uint32_t rate) {
	if (is_running)
		return tsdrplugin_getsamplerate();

	req_rate = rate;

	try {
		int r = rtlsdr_set_sample_rate(dev, req_rate);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set sample rate.", TSDR_CANNOT_OPEN_DEVICE);
		}
		int real_rate = rtlsdr_get_sample_rate(dev);
		req_rate = real_rate;
	}
	catch (std::exception const&  ex)
	{
	}

	return req_rate;
}

EXTERNC TSDRPLUGIN_API uint32_t __stdcall tsdrplugin_getsamplerate() {

	try {
		req_rate = rtlsdr_get_sample_rate(dev);
	}
	catch (std::exception const&  ex)
	{
	}

	return req_rate;
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setbasefreq(uint32_t freq) {
	req_freq = freq;

	try {
		int r = rtlsdr_set_center_freq(dev, req_freq);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set center frequency.", TSDR_CANNOT_OPEN_DEVICE);
		}
	}
	catch (std::exception const&  ex)
	{
	}

	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_stop(void) {
	is_running = 0;
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_setgain(float gain) {
	req_gain = gain;
	int gain_val = nearest_gain(dev, tortlsdrgain(req_gain));
	//verbose_gain_set(dev, gain_val);
	int r = rtlsdr_set_tuner_gain_mode(dev, 1);
	if (r < 0) {
		RETURN_EXCEPTION("Can't set manual gain mode.", TSDR_CANNOT_OPEN_DEVICE);
	}
		
	r = rtlsdr_set_tuner_gain(dev, gain_val);
	if (r < 0) {
		RETURN_EXCEPTION("Can't set gain.", TSDR_CANNOT_OPEN_DEVICE);
	}
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API int __stdcall tsdrplugin_readasync(tsdrplugin_readasync_function cb, void *ctx) {

	uint8_t *buff = NULL;
	float *floatbuff = NULL;
	int n_read, r;

	is_running = 1;

	r = rtlsdr_set_sample_rate(dev, req_rate);
	if (r < 0) {
			RETURN_EXCEPTION("Can't set sample rate.", TSDR_CANNOT_OPEN_DEVICE);
	}

	r = rtlsdr_set_center_freq(dev, req_freq);
	if (r < 0) {
		RETURN_EXCEPTION("Can't set center frequency.", TSDR_CANNOT_OPEN_DEVICE);
	}

	if (0 == req_gain) {
                  /* Enable automatic gain */
		//verbose_auto_gain(dev);
		r = rtlsdr_set_tuner_gain_mode(dev, 0);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set auto gain mode.", TSDR_CANNOT_OPEN_DEVICE);
		}
	} else {
		/* Enable manual gain */
		int gain_val = nearest_gain(dev, tortlsdrgain(req_gain));
		//verbose_gain_set(dev, gain_val);
		r = rtlsdr_set_tuner_gain_mode(dev, 1);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set manual gain mode.", TSDR_CANNOT_OPEN_DEVICE);
		}
			
		r = rtlsdr_set_tuner_gain(dev, gain_val);
		if (r < 0) {
			RETURN_EXCEPTION("Can't set gain.", TSDR_CANNOT_OPEN_DEVICE);
		}
	}

	//verbose_reset_buffer(dev);
	r = rtlsdr_reset_buffer(dev);
	if (r < 0) {
		RETURN_EXCEPTION("Can't reset buffer.", TSDR_CANNOT_OPEN_DEVICE);
	}

	//size_t buff_size = HOW_OFTEN_TO_CALL_CALLBACK_SEC * req_rate * 2;
	size_t buff_size = (16 * 16384);
	buff = (uint8_t *)malloc(buff_size * sizeof(uint8_t));
	floatbuff = (float *)malloc(buff_size * sizeof(float));
	while(is_running){
		r = rtlsdr_read_sync(dev, buff, buff_size, &n_read);
		if (r < 0) {
			RETURN_EXCEPTION("rtlsdr_read_sync failed.", TSDR_CANNOT_OPEN_DEVICE);
		}
		if (n_read==buff_size){
			for(int i=0;i<n_read;i++){
				floatbuff[i]=(float)buff[i]/128.0;
			}
			cb(floatbuff, n_read, ctx, 0);
		
		}
		else{
			RETURN_EXCEPTION("short read.", TSDR_CANNOT_OPEN_DEVICE);
			cb(floatbuff, 0, ctx, n_read);
			is_running=0;
		}
			
	}
		
	if (buff!=NULL) free(buff);
	if (floatbuff!=NULL) free(floatbuff);
	RETURN_OK();

	return 0; // to avoid getting warning from stupid Eclpse
}

EXTERNC TSDRPLUGIN_API void __stdcall tsdrplugin_cleanup(void) {

	rtlsdr_close(dev);

	is_running = 0;
}
