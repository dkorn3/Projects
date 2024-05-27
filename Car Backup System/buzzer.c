/******************************************************************************

 * buzzer.c: This file contains the initialization function for the buzzer.
 *****************************************************************************/

#include "buzzer.h"

/******************************************************************************
 * Initialize the buzzer
 * Pin usage: Grove base port J17, Tiva C PC5 (Port C, Pin 5)
 *****************************************************************************/
void BuzzerInit()
{
    // Enable Timer 0 and Timer 1
    SysCtlPeripheralEnable(SYSCTL_PERIPH_WTIMER0);
    SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOC);

    // Connect pins to those timers
    GPIOPinTypeGPIOOutput(GPIO_PORTC_BASE , GPIO_PIN_5);
    GPIOPinTypeTimer(GPIO_PORTC_BASE , GPIO_PIN_5);
    GPIOPinConfigure(GPIO_PC5_WT0CCP1);


    // Select PWM for Timer 0 sub-Timer B,
    TimerConfigure(WTIMER0_BASE, (TIMER_CFG_SPLIT_PAIR | TIMER_CFG_B_PWM));

    // Invert the PWM waveform, so that the Match register value is the pulse width.
    // Otherwise, the pulse width will be (Load value) - (Match value).
    // The inversion is done by enabling output inversion on the PWM pins.
    TimerControlLevel(WTIMER0_BASE, TIMER_B, true /* output inversion */);

    // Enable the Timer 0's TimerB and Timer 1's TimerA and TimerB
    TimerEnable(WTIMER0_BASE, TIMER_B);
    //TimerEnable(TIMER1_BASE, TIMER_A | TIMER_B);
}
void MusicSetBuzzer(int pitch, int tone)
{
    int buzzer = 50000000 /329.63;
    int v=tone;
    PwmBuzzerSet(buzzer, v);
}
void MusicTurnOffBuzzer()
{
    PwmBuzzerSet(1000,0);
}


void PwmBuzzerSet(int PWMfreq, int PWMdCycle)
{
    // Set the PWM parameters for red LED
    TimerLoadSet(WTIMER0_BASE, TIMER_B, PWMfreq);
    TimerMatchSet(WTIMER0_BASE, TIMER_B, PWMdCycle);

}

