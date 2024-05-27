/*
 * lab project main file
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <math.h>
#include "launchpad.h"
#include "seg7.h"
#include "ranger.h"
#include "buzzer.h"


/*
 * Global variables
 */
Seg7Display seg7 = { { 0, 0, 0, 0 }, true/* colon on */};

typedef struct
{
    bool LED_on;  // if the LED is turned on
    bool buzzer_on;
    double data;

} SysState;

SysState sys =
{
    false,              // LED turned off,
    false

};


// The events

Event trigger_ranger_reading_event;
Event check_ranger_event;
Event led_event;
Event buzzer_play_event;





/*******************************************
 * Task 1: Trigger and read the ranger
 ******************************************/

// Trigger the ranger reading every 0.5 seconds

void FlashLED(Event *event)
{
    int delay=0;
       double conversion = (( sys.data/ 50000000 ) * 340)/2 ;
       double mili = (conversion*1000);
    if(mili > 920.0)
    {
        sys.LED_on = true;
    }
    else
    {
        delay= (1000.0 * (mili/921.0));
             // see ledTurnOnOff() in launchpad.h

    }

    // If on-board LED is turned on, turn it off
    // Otherwise, turn it on with the current color choice
    if (sys.LED_on)
    {
        // Turn off the three sub-LEDs
        LedTurnOnOff(false, false, false);          // see ledTurnOnOff() in launchpad.h
        sys.LED_on = false;
    }
    else
    {

        LedTurnOnOff(true, false, false);
        sys.LED_on = true;
    }

    // Schedule an event to self in delay ms
    EventSchedule(event, event->time +delay);
}




void BuzzerPlay(Event *event)                   // the scheduled time
{

       int delay=0;
        int volume=0;
           double conversion = (( sys.data/ 50000000 ) * 340)/2 ;
           double mili = (conversion*1000);
        if (mili>920.0)
        {

            volume=0;
        }
        else{
       double vol = (151000 * (1-(mili/921)));
       delay= (1000.0 * (mili/921.0));
        volume = (int)vol;
        }



    if (sys.buzzer_on)
            {
                // The buzzer is on: turn it off
               MusicTurnOffBuzzer();
                sys.buzzer_on = false;
                                   // off for 2988 ms
            }
            else
            {
                // The buzzer is off: turn it on
                MusicSetBuzzer(1000, volume);
                sys.buzzer_on = true;
                                        // on for 12 ms
            }



    // schedule callback to buzzer play
    EventSchedule(event, event->time + delay);
}



void UpdateSeg7(double mili)
{
    Seg7Display seg7;


    double decibel=0;

    if (mili>920)
         {
             decibel=0;

         }
         else{
        decibel = (85 * (1-(mili/921)));

         }
    int dB= decibel*100.0;

    seg7.colon_on = true;

                 if (dB< 1000 && dB>=100)
                 {
                     seg7.digit[3] = 10;
                     seg7.digit[2] = (dB  / 100);
                     seg7.digit[1] = ((dB/ 10) % 10);
                     seg7.digit[0] =dB % 10;
                 }
                  if (dB < 100 && dB>=10)
                 {
                     seg7.digit[3] = 10;
                     seg7.digit[2] = 10;
                     seg7.digit[1] = (dB / 10) % 10;
                     seg7.digit[0] = dB % 10;
                 }
                  if (dB < 10)
                  {
                     seg7.digit[3] =  10;
                     seg7.digit[2] = 10;
                     seg7.digit[1] = 0;
                     seg7.digit[0] =dB % 10;
                 }
                  if(dB>=1000)
                 {
                     seg7.digit[3] = dB / 1000;
                     seg7.digit[2] =(dB/100)%10;
                     seg7.digit[1] = (dB/ 10) % 10;
                     seg7.digit[0] = dB % 10;
                 }

    // Display the distance
    Seg7Update(&seg7);
}




// Trigger the ranger reading
void TriggerRangerReading(Event *event)
{
    RangerTriggerReading();

    EventSchedule(event, event->time + 500);
}

// Check and show ranger reading
void CheckRanger(Event *event)
{

    // Get ranger data and convert it to distance in millimeter
   sys.data = RangerGetData();

    double conversion = ((sys.data/ 50000000 ) * 340)/2 ;
    double mili = (conversion*1000);

    // Update the distance on 7-seg
    UpdateSeg7(mili);


}


/*******************************************
 * The main function
 ******************************************/
void main(void)
{
    // Initialize the LaunchPad and peripherals
    LaunchPadInit();
    Seg7Init();
    RangerInit();
    BuzzerInit();

    // Initialize the events
    EventInit(&trigger_ranger_reading_event, TriggerRangerReading);
    EventInit(&check_ranger_event, CheckRanger);
    EventInit(&buzzer_play_event, BuzzerPlay);
    EventInit(&led_event, FlashLED);


    // Register ISR events
    RangerEventRegister(&check_ranger_event);


    // Schedule time event
    EventSchedule(&trigger_ranger_reading_event, 100);
    EventSchedule(&buzzer_play_event,100);
    EventSchedule(&led_event, 100);

    while (true)
    {
        // Wait for interrupt
        asm("   wfi");

        // Execute scheduled callbacks
        EventExecute();
    }
}
