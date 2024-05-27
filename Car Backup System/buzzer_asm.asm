 	.cdecls "buzzer.h"

	 .text




_SYSCTL_PERIPH_WTIMER0  .field   SYSCTL_PERIPH_WTIMER0
_SYSCTL_PERIPH_GPIOC    .field  SYSCTL_PERIPH_GPIOC
_GPIO_PC5_WT0CCP1       .field  GPIO_PC5_WT0CCP1
_WTIMER0_BASE          .field   WTIMER0_BASE
_GPIO_PORTC_BASE     .field  GPIO_PORTC_BASE
_GPIO_PIN_5          .field   GPIO_PIN_5
;******************************************************************************
			.global BuzzerInit
        .asmfunc
BuzzerInit
		PUSH{LR}
		MOVW r0,#0x5c00
		MOVT r0,#0xf000
        BL  SysCtlPeripheralEnable
        MOVW r0,#0x0802
		MOVT r0,#0xf000
        BL  SysCtlPeripheralEnable
        ;////////////////////////////////
       MOVW r0,#0x6000
		MOVT r0,#0x4000
		MOVW r1,#0x0020
		MOVT r1,#0x0000
        BL  GPIOPinTypeGPIOOutput
        MOVW r0,#0x6000
		MOVT r0,#0x4000
		MOVW r1,#0x0020
		MOVT r1,#0x0000
        BL    GPIOPinTypeTimer
		MOVW r0,#0x1407
		MOVT r0,#0x0002
        BL    GPIOPinConfigure
        ;//////////////////////////
        MOVW r0,#0x6000
		MOVT r0,#0x4003
		MOVW r2,#0x0000
		MOVT r2,#0x0400
		MOVW r3,#0x0A00
		MOVT r3,#0x0000
        ORR   r1,r2,r3
        BL   TimerConfigure
        ;//////////////////////////

        MOVW r0,#0x6000
		MOVT r0,#0x4003
		MOVW r1,#0xff00
		MOVT r1,#0x0000
        MOV  r2,#1
         BL  TimerControlLevel

         ;/////////////////////////
           MOVW r0,#0x6000
		MOVT r0,#0x4003
		 MOVW r1,#0xff00
		MOVT r1,#0x0000
         BL   TimerEnable
		POP{PC}
        .endasmfunc

  ;////////////////////////////////

			.global PwmBuzzerSet
        .asmfunc
PwmBuzzerSet
		 PUSH{LR}
		 MOV r2,r0
		 MOV r3,r1
		   MOVW r0,#0x6000
		MOVT r0,#0x4003
		 MOVW r1,#0xff00
		MOVT r1,#0x0000

		 BL   TimerLoadSet
		   MOVW r0,#0x6000
		MOVT r0,#0x4003
		 MOVW r1,#0xff00
		MOVT r1,#0x0000
		MOV r2,r3


		 BL   TimerMatchSet
		POP{PC}
        .endasmfunc
;/////////////////////////////////////
			.global MusicSetBuzzer
        .asmfunc
MusicSetBuzzer
		 PUSH{LR}
		;MOVW r0,#151685
		MOVW r0,#0x5085
		MOVT r0,#0x0002
		 BL   PwmBuzzerSet
		POP{PC}
        .endasmfunc

 ;/////////////////////////////////////////////////
 			.global MusicTurnOffBuzzer
        .asmfunc
MusicTurnOffBuzzer
		 PUSH{LR}

		MOV r0,#1000
		MOV r1,#0
	 BL   PwmBuzzerSet
		POP{PC}
        .endasmfunc
