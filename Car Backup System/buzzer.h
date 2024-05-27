/*
 * buzzer.h: Header file for buzzer-related functions
 *
 */

#ifndef BUZZER_H_
#define BUZZER_H_

#include <stdbool.h>
#include <stdint.h>
#include <inc/hw_memmap.h>
#include <inc/hw_timer.h>
#include <driverlib/sysctl.h>
#include <driverlib/gpio.h>
#include <driverlib/pin_map.h>
#include <driverlib/timer.h>

// Initialize the buzzer
void BuzzerInit();

void MusicSetBuzzer(int pitch, int tone);
void MusicTurnOffBuzzer();


void PwmBuzzerSet(int PWMfreq, int PWMdCycle);

#endif /* BUZZER_H_ */
