/*
	Copyright (C) 2013 CurlyMo

	This file is part of pilight.

	pilight is free software: you can redistribute it and/or modify it under the 
	terms of the GNU General Public License as published by the Free Software 
	Foundation, either version 3 of the License, or (at your option) any later 
	version.

	pilight is distributed in the hope that it will be useful, but WITHOUT ANY 
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR 
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdint.h>

#include "../../pilight.h"
#include "common.h"
#include "log.h"
#include "threads.h"
#include "protocol.h"
#include "hardware.h"
#include "binary.h"
#include "gc.h"
#include "json.h"
#include "dht22.h"
#include "../pilight/wiringPi.h"
	
#define MAXTIMINGS 85

unsigned short dht22_loop = 1;

static uint8_t sizecvt(const int read_value)
{
	/* digitalRead() and friends from wiringpi are defined as returning a value
	   < 256. However, they are returned as int() types. This is a safety function */
	if(read_value > 255 || read_value < 0) {
		logprintf(LOG_NOTICE, "invalid data from wiringPi library");
	}
  
	return (uint8_t)read_value;
}

void *dht22Parse(void *param) {

	struct JsonNode *json = (struct JsonNode *)param;
	struct JsonNode *jsettings = NULL;
	int interval = 5;
	int dht_pin = 7;

	json_find_number(json, "gpio", &dht_pin);
	if((jsettings = json_find_member(json, "settings"))) {
		json_find_number(jsettings, "interval", &interval);
	}
	json_delete(json);	
	
	while(dht22_loop) {

		int tries = 5;
		unsigned short got_correct_date = 0;

		while(tries && !got_correct_date) {

			uint8_t laststate = HIGH;
			uint8_t counter = 0;
			uint8_t j = 0, i = 0;

			int dht22_dat[5] = {0,0,0,0,0};

			// pull pin down for 18 milliseconds
			pinMode(dht_pin, OUTPUT);
			digitalWrite(dht_pin, LOW);
			delay(18);
			// then pull it up for 40 microseconds
			digitalWrite(dht_pin, HIGH);
			delayMicroseconds(40);
			// prepare to read the pin
			pinMode(dht_pin, INPUT);

			// detect change and read data
			for(i=0; i<MAXTIMINGS; i++) {
				counter = 0;
				while(sizecvt(digitalRead(dht_pin)) == laststate) {
					counter++;
					delayMicroseconds(1);
					if (counter == 255) {
						break;
					}
				}
				laststate = sizecvt(digitalRead(dht_pin));

				if(counter == 255) 
					break;

				// ignore first 3 transitions
				if((i >= 4) && (i%2 == 0)) {
					// shove each bit into the storage bytes
					dht22_dat[j/8] <<= 1;
					if (counter > 16)
						dht22_dat[j/8] |= 1;
						j++;
				  	}
				}

			// check we read 40 bits (8bit x 5 ) + verify checksum in the last byte
			// print it out if data is good
			if((j >= 40) && (dht22_dat[4] == ((dht22_dat[0] + dht22_dat[1] + dht22_dat[2] + dht22_dat[3]) & 0xFF))) {

				got_correct_date = 1;

				int h = dht22_dat[0] * 256 + dht22_dat[1];
				int t = (dht22_dat[2] & 0x7F)* 256 + dht22_dat[3];

				if((dht22_dat[2] & 0x80) != 0) 
					t *= -1;

				
				dht22->message = json_mkobject();
				JsonNode *code = json_mkobject();
				json_append_member(code, "id", json_mkstring(dht22->id));
				json_append_member(code, "temperature", json_mknumber(t));
				json_append_member(code, "humidity", json_mknumber(h));

				json_append_member(dht22->message, "code", code);
				json_append_member(dht22->message, "origin", json_mkstring("receiver"));
				json_append_member(dht22->message, "protocol", json_mkstring(dht22->id));
				pilight.broadcast(dht22->id, dht22->message);
				json_delete(dht22->message);
				dht22->message = NULL;
			} else {
				logprintf(LOG_DEBUG, "dht22 data checksum was wrong");
				tries--;
				sleep(1);
			}
		}
		sleep((unsigned int)interval);
	}

	return (void *)NULL;
}

void dht22InitDev(JsonNode *jdevice) {
	char *output = json_stringify(jdevice, NULL);
	JsonNode *json = json_decode(output);
	threads_register("dht22", &dht22Parse, (void *)json);
	sfree((void *)&output);
}

int dht22GC(void) {
	dht22_loop = 0;
	return 1;
}

void dht22Init(void) {
	gc_attach(dht22GC);

	protocol_register(&dht22);
	protocol_set_id(dht22, "dht22");
	protocol_device_add(dht22, "dht22", "1-wire temperature and humidity sensor");
	protocol_device_add(dht22, "dht11", "1-wire temperature and humidity sensor");
	protocol_device_add(dht22, "am2302", "1-wire temperature and humidity sensor");	
	dht22->devtype = WEATHER;
	dht22->hwtype = SENSOR;

	options_add(&dht22->options, 't', "temperature", has_value, config_value, "^[0-9]{1,3}$");
	options_add(&dht22->options, 'h', "humidity", has_value, config_value, "^[0-9]{1,3}$");
	options_add(&dht22->options, 'i', "id", has_value, config_id, ".+");
	options_add(&dht22->options, 'g', "gpio", has_value, config_value, "^[0-9]{1,2}$");

	protocol_setting_add_number(dht22, "decimals", 1);
	protocol_setting_add_number(dht22, "humidity", 1);
	protocol_setting_add_number(dht22, "temperature", 1);
	protocol_setting_add_number(dht22, "battery", 0);
	protocol_setting_add_number(dht22, "interval", 5);

	dht22->initDev=&dht22InitDev;
	
	wiringPiSetup();
}