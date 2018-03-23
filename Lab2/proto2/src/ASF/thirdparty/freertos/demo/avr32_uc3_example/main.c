/*****************************************************************************
* Auteur : Maxime Turenne
* Copyright : Maxime Turenne
* Description: Demo d'utilisation de FreeRTOS dans le cadre d'un thermostate num�rique.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Environment header files. */
#include "power_clocks_lib.h"

#include "board.h"
#include "compiler.h"
#include "dip204.h"
#include "intc.h"
#include "gpio.h"
#include "pm.h"
#include "delay.h"
#include "spi.h"
#include "conf_clock.h"
#include "adc.h"
#include "usart.h"
//#include <print_funcs.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"

// tasks
void vUART_Cmd_RX(void *pvParameters);
void vADC_Cmd(void *pvParameters);
void vUART_SendSample(void *pvParameters);
void vLED_Flash(void *pvParameters);
void vAlarmMsgQ(void *pvParameters);
	
// initialization functions
void initialiseUSART(void);
void initialiseADC(void);

char queueIsFull();

// global var
volatile char ACQUISITION = 0;
volatile char ALERT = 0;
volatile char START = 0;
volatile char QUEUE_IS_FULL = 0;
volatile char EMPTYING_QUEUE = 0;
volatile avr32_adc_t *adc = &AVR32_ADC;
volatile xQueueHandle handlePotMessageQueue;
volatile xQueueHandle handlelightMessageQueue;

//TaskHandle
volatile xTaskHandle alertMessageQueueHandle;
volatile xTaskHandle ledFlashHandle;


// semaphore
static xSemaphoreHandle START_SEMAPHORE = NULL;
static xSemaphoreHandle POT_MESSAGE_QUEUE_SEMAPHORE = NULL;
static xSemaphoreHandle FULL_QUEUE_SEMAPHORE = NULL;
static xSemaphoreHandle LIGHT_MESSAGE_QUEUE_SEMAPHORE = NULL;
static xSemaphoreHandle ACQUISITION_SEMAPHORE = NULL;
static xSemaphoreHandle ALERT_SEMAPHORE = NULL;



int main(void) {
	pm_switch_to_osc0(&AVR32_PM, FOSC0, OSC0_STARTUP);
	initialiseUSART();
	initialiseADC();

	START_SEMAPHORE = xSemaphoreCreateCounting(1,1);
	POT_MESSAGE_QUEUE_SEMAPHORE = xSemaphoreCreateCounting(1,1);
	LIGHT_MESSAGE_QUEUE_SEMAPHORE = xSemaphoreCreateCounting(1,1);
	FULL_QUEUE_SEMAPHORE = xSemaphoreCreateCounting(1,1);
	ACQUISITION_SEMAPHORE = xSemaphoreCreateCounting(1,1);
	ALERT_SEMAPHORE = xSemaphoreCreateCounting(1,1);


	
	// J'ai pris 500 au hasard ...
	handlePotMessageQueue = xQueueCreate(500, sizeof( char ) );
	handlelightMessageQueue = xQueueCreate(500, sizeof( char ) );
 
	// TODO: Calculer les grosseurs des stacks
	// TODO: Definir les priorites
	xTaskCreate(vUART_Cmd_RX, (signed char*)"Receive", configMINIMAL_STACK_SIZE*2, NULL, tskIDLE_PRIORITY +1, NULL );
	xTaskCreate(vADC_Cmd, (signed char*)"Conversion", configMINIMAL_STACK_SIZE*2, NULL,tskIDLE_PRIORITY + 2, NULL );
	xTaskCreate(vUART_SendSample, (signed char*)"Send", configMINIMAL_STACK_SIZE*2, NULL,tskIDLE_PRIORITY + 3, NULL );
	xTaskCreate(vLED_Flash, (signed char*)"LED Flash", configMINIMAL_STACK_SIZE*2, NULL, tskIDLE_PRIORITY +4 , NULL );

	/* Start the scheduler. */
	vTaskStartScheduler();

	/* Will only get here if there was insufficient memory to create the idle task. */
	return 0;
}



/************************************************************************/
/* Check whether or not the queue is full                                */
/************************************************************************/
char queueIsFull(){
	char isFull = 0;
	
	xSemaphoreTake(POT_MESSAGE_QUEUE_SEMAPHORE, portMAX_DELAY);
	if(xQueueIsQueueFullFromISR(handlePotMessageQueue) == 0){
		isFull = 1;
	}
	xSemaphoreGive(POT_MESSAGE_QUEUE_SEMAPHORE);
	
	// ONLY NEED TO VERIFY ONE QUEUE SINCE THEY FILL UP AT THE SAME RATE ??????
	
	//xSemaphoreTake(LIGHT_MESSAGE_QUEUE_SEMAPHORE, portMAX_DELAY);
	//if(xQueueIsQueueFullFromISR(handlelightMessageQueue) == 0){
		//isFull = 1;
	//}
	//xSemaphoreGive(LIGHT_MESSAGE_QUEUE_SEMAPHORE);
	
	//Create AlertMsgQ only when the queue is full and theres been no ALERT
	if(isFull && ALERT != 1){
		xTaskCreate(vAlarmMsgQ, (signed char*)"Alert Message Queue", configMINIMAL_STACK_SIZE*2, NULL, tskIDLE_PRIORITY + 5, &alertMessageQueueHandle );
	}
	
	
	return isFull;
}



/************************************************************************/
/*	Vide le � Message Queue � et envoi les �chantillons au UART pour une
/*	transmission en direction du PC.                                    */
/************************************************************************/
void vUART_SendSample(void *pvParameters) {
	char potValue = 0;
	char lightValue = 0;
	int lightResponse = 0;
	int potResponse = 0;

	
	while(1){
		xSemaphoreTake(POT_MESSAGE_QUEUE_SEMAPHORE, portMAX_DELAY);
		potResponse = xQueueReceive( handlePotMessageQueue, &potValue, ( portTickType ) 10 );
		xSemaphoreGive(POT_MESSAGE_QUEUE_SEMAPHORE);
		
		if(potResponse){
			usart_write_char(&AVR32_USART1, potValue);
		}
		
		xSemaphoreTake(LIGHT_MESSAGE_QUEUE_SEMAPHORE, portMAX_DELAY);
		lightResponse = xQueueReceive( handlelightMessageQueue, &lightValue, ( portTickType ) 10 );
		xSemaphoreGive(LIGHT_MESSAGE_QUEUE_SEMAPHORE);
		
		if(lightResponse){
			usart_write_char(&AVR32_USART1, lightValue);
		}
		
		if(lightValue || potResponse){
			xSemaphoreTake(ACQUISITION_SEMAPHORE, portMAX_DELAY);
			ACQUISITION = 1;
			xSemaphoreGive(ACQUISITION_SEMAPHORE);			
		}else{
			xSemaphoreTake(ACQUISITION_SEMAPHORE, portMAX_DELAY);
			ACQUISITION = 0;
			xSemaphoreGive(ACQUISITION_SEMAPHORE);
		}
		vTaskDelay(10);
	}
}



/*********************************************************************************
/*  Cette t�che d�marre les conversions, obtient les �chantillons num�ris�s et les
/*  place dans le � Message Queue �. Ceci doit �tre fait � la bonne vitesse.
/*    
/*	Si le � Message Queue � est plein, envoie l�information � la t�che
/*	AlarmMsgQ()./*********************************************************************************/	
void vADC_Cmd(void *pvParameters) {
	char adc_value_pot;
	char adc_value_light;
	char tempStart;
	
	while(1){
		
		xSemaphoreTake(START_SEMAPHORE, portMAX_DELAY);
		tempStart = START;
		xSemaphoreGive(START_SEMAPHORE);
		
		if(tempStart){
			adc_start(adc);
			adc_value_pot = (char) (adc_get_value(adc, ADC_POTENTIOMETER_CHANNEL) >> 2) & 0b11111110;
			adc_start(adc);
			adc_value_light = (char) (adc_get_value(adc, ADC_LIGHT_CHANNEL) >> 2) | 0x01;

			// Add data to the proper queue
			xSemaphoreTake(POT_MESSAGE_QUEUE_SEMAPHORE, portMAX_DELAY);
			xQueueSend( handlePotMessageQueue, ( void * ) &adc_value_pot, ( portTickType ) 10 );
			xSemaphoreGive(POT_MESSAGE_QUEUE_SEMAPHORE);
			
			xSemaphoreTake(LIGHT_MESSAGE_QUEUE_SEMAPHORE, portMAX_DELAY);
			xQueueSend( handlelightMessageQueue, ( void * ) &adc_value_light, ( portTickType ) 10 );
			xSemaphoreGive(LIGHT_MESSAGE_QUEUE_SEMAPHORE);
			
			// Queue is full, raise flag so the led can be light up
			if(queueIsFull()){
				//xSemaphoreTake(FULL_QUEUE_SEMAPHORE, portMAX_DELAY);
				//QUEUE_IS_FULL = 1;
				//xSemaphoreGive(FULL_QUEUE_SEMAPHORE);
			}
		}
		vTaskDelay(10);
	}
}



/************************************************************************
	V�rifie, � chaque 200msec, si des commandes sont re�ues par le UART.
	Si une commande est re�ue, traiter celle-ci et envoyer l�ordre d�arr�t ou de
	d�part � la t�che ADC_Cmd().
/************************************************************************/
void vUART_Cmd_RX(void *pvParameters) {
	int char_recu = 'd';

	while (1) {
		usart_read_char(&AVR32_USART1, &char_recu);
		
		if(char_recu == 's'){
			xSemaphoreTake(START_SEMAPHORE, portMAX_DELAY);
			START = 1;
			xSemaphoreGive(START_SEMAPHORE);
		}
		else if(char_recu == 'x'){
			xSemaphoreTake(START_SEMAPHORE, portMAX_DELAY);
			START = 0;
			xSemaphoreGive(START_SEMAPHORE);
		}
		
		vTaskDelay(200);
	}
}

/************************************************************************/
/* Cette t�che est r�veill� seulement si un d�bordement de la � Message Queue �
survient. Elle commande l�allumage du LED3 en informant la t�che
LED_Flash().                                                                     */
/************************************************************************/
void vAlarmMsgQ(void *pvParameters){
	unsigned portBASE_TYPE ledFlashPriority;
	
	ledFlashPriority = uxTaskPriorityGet(ledFlashHandle);
	while(1){
		
		xSemaphoreTake(ALERT_SEMAPHORE, portMAX_DELAY);
		ALERT = 1;
		xSemaphoreGive(ALERT_SEMAPHORE);
		
		//Change priority of LED_FLASH to trigger it now
		vTaskPrioritySet(ledFlashHandle, ledFlashPriority  - 1 );
		
		
		vTaskDelay(1000);
	}
}


/************************************************************************/
/* Clignotement des LEDs
Effectue le clignotement des LEDs au 200msec.
? LED1 clignote toujours d�s que votre microcontr�leur est aliment�.
? LED2 clignote lorsque l�acquisition est en service.
? LED3 s�allume et reste allum� si le � Message Queue � d�borde au
moins une fois.                                        */
/************************************************************************/
void vLED_Flash(void *pvParameters) {
 		while(1){
			//Clignotement  alimentation
			LED_Toggle(LED0);
			
			//Clignotement acquisition
			if(START){
			 	LED_Toggle(LED1);
			}else 
				LED_Off(LED1);

			//Clignotement Message Queue deborde
			if(ALERT){
				LED_On(LED2);//On board LED3
				vTaskDelete(alertMessageQueueHandle);
			}
		
			vTaskDelay(100);

		}
	
}

/************************************************************************
	Fonction that initialise ADC
/************************************************************************/
void initialiseADC(void){
	static const gpio_map_t ADC_GPIO_MAP = { 
		{ ADC_LIGHT_PIN,			ADC_LIGHT_FUNCTION }, 
		{ ADC_POTENTIOMETER_PIN,	ADC_POTENTIOMETER_FUNCTION } 
	};

	gpio_enable_module(ADC_GPIO_MAP, 1);

	AVR32_ADC.mr |= 0x1 << AVR32_ADC_MR_PRESCAL_OFFSET;
	adc_configure(adc);

	// Enable the ADC channels.
	adc_enable(adc, ADC_POTENTIOMETER_CHANNEL);
	adc_enable(adc, ADC_LIGHT_CHANNEL);
}



/************************************************************************
	Fonction that initialise USART                                       
/************************************************************************/
void initialiseUSART(void){
	static const gpio_map_t USART_GPIO_MAP =
	{
		{AVR32_USART1_RXD_0_0_PIN, AVR32_USART1_RXD_0_0_FUNCTION},
		{AVR32_USART1_TXD_0_0_PIN, AVR32_USART1_TXD_0_0_FUNCTION}
	};

	static const usart_options_t USART_OPTIONS =
	{
		.baudrate     = 57600,
		.charlength   = 8,
		.paritytype   = USART_NO_PARITY,
		.stopbits     = USART_1_STOPBIT,
		.channelmode  = USART_NORMAL_CHMODE
	};

	// Assign GPIO to USART.
	gpio_enable_module(USART_GPIO_MAP,
	sizeof(USART_GPIO_MAP) / sizeof(USART_GPIO_MAP[0]));

	// Initialize USART in RS232 mode.
	usart_init_rs232(&AVR32_USART1, &USART_OPTIONS, FOSC0);
}