/*
 * range.h: Starter code for ECE 266 Lab 8, main.c, spring 2024
 *
 * Lab 8: Distance Measurement
 *
 * Created by Zhao Zhang
 * Last update: 3/14/2024
 */


#ifndef RANGER_H_
#define RANGER_H_

#include <stdint.h>
#include <stdbool.h>
#include <inc/hw_memmap.h>
#include <inc/hw_ints.h>
#include <driverlib/sysctl.h>
#include <driverlib/gpio.h>
#include <driverlib/pin_map.h>
#include <driverlib/timer.h>
#include "launchpad.h"

// Initialize the ranger. Assume it is connected to Port C Pin 4, Grove jumper J16 (SIG to pin 37).
// Use wide timer 0 (32-bit/64-bit).
void RangerInit();

// Send start pulse
void RangerTriggerReading();

// Check if any new ranger data is ready (used for polling)
bool RangerDataReady();

// Read the ranger data recorded by ISR
uint32_t RangerGetData();

// Register an event object for the ISR to trigger
void RangerEventRegister(Event *event);


#endif /* RANGER_H_ */
