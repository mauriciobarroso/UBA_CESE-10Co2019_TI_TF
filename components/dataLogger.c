/*
 * data_logger.c
 *
 * Created on: Nov 1, 2019
 * Author: Mauricio Barroso
 */

/*==================[inlcusions]============================================*/

#include "dataLogger.h"

/*==================[macros]=================================================*/

/* mask to configure interrupt pins */
#define INTERRUPT_PINS_MASK	( ( 1ULL << GPIO_PULSES ) | ( 1ULL << GPIO_ALARM ) )	/*!< data logger mask pin */

/* addresses */
#define DEVICE_SETTINGS_ADDR		0x0		/*!< direccion en la eeprom donde se guarda los device settings */
#define USER_ID_ADDR				0x10	/*!< direccion en la eeprom donde se guarda el user ID */
#define MONTHLY_PULSES_ADDR			0x1A	/*!< direccion en la eeprom donde se guarda el user ID */
#define CURRENT_INDEX_ADDR			0x20	/*!< dirección en la eeprom donde se guarda la fecha */
#define	TOTAL_LOGGED_DAYS_ADDR		0x22	/*!< dirección en la eeprom donde se guarda la cantidad de dias monitoreados */
#define	MONTHLY_LOGGED_DAYS_ADDR	0x24	/*!< dirección en la eeprom donde se guarda la cantidad de dias monitoreados */

/* values */
#define BASE_INDEX					0x30	/*!< índice base para el guardado de pulsos */
#define DATA_SIZE					5		/*!< tamaño en Bytes de los datos guardados en la EEPROM */

/*==================[typedef]================================================*/

/*==================[internal data declaration]==============================*/

/* tag for debug */
static const char * TAG = "data_logger";

/*==================[external data declaration]==============================*/

/* handler tasks */
static TaskHandle_t pulses_handle, alarm_handle;

/*==================[internal functions declaration]=========================*/

/* isr handlers */
static void IRAM_ATTR alarm_isr( void *pvParameters );
static void IRAM_ATTR pulses_isr( void *pvParameters );

/* function tasks */
static void pulses_task( void * pvParameters );
static void alarm_task( void * pvParameters );

/*==================[external functions definition]=========================*/


void data_logger_init ( data_logger_t * const me ) {
	/* se inicializa i2c interface */
	i2c_init();

	ds3231_clear_alarm_flag( 1 );

	/* reset
	uint8_t data = 0;
	for( uint8_t i = 0; i < 0xFF; i++ )
		at24c32_write8( i, &data );

	me->rtc.date.date= 0x8;
	me->rtc.date.month = 0x5;
	me->rtc.date.year = 0x20;
	ds3231_set_date( &me->rtc );*/

	/* inicialización de la variable para el índice de conteo de pulsos */
	at24c32_read16( CURRENT_INDEX_ADDR, &me->index );
	if( me->index < BASE_INDEX || me->index == 0xFFFF )
		me->index = BASE_INDEX;
	#ifdef DEBUG_MODE
		ESP_LOGI( TAG, "index:0x%X", me->index );
	#endif

	/* inicialización de la variable para el conteo de pulsos */
	at24c32_read16( me->index, &me->daily_pulses );
	if( me->daily_pulses == 0xFFFF )
		me->daily_pulses = 0;
	#ifdef DEBUG_MODE
		ESP_LOGI( TAG, "daily_pulses:%d", me->daily_pulses );
	#endif

	/* inicialización de la variable para el conteo de días monitoreados */
	at24c32_read16( TOTAL_LOGGED_DAYS_ADDR, &me->total_logged_days );
	if( me->total_logged_days == 0xFF )
		me->total_logged_days = 0;
	#ifdef DEBUG_MODE
		ESP_LOGI( TAG, "total_logged_days:%d", me->total_logged_days );
	#endif

	/* inicialización de la variable para el conteo de días monitoreados */
	at24c32_read16( MONTHLY_LOGGED_DAYS_ADDR, &me->monthly_logged_days );
	if( me->monthly_logged_days == 0xFF )
		me->monthly_logged_days = 0;
	#ifdef DEBUG_MODE
		ESP_LOGI( TAG, "monthly_logged_days:%d", me->monthly_logged_days );
	#endif

	/* se inicializala variable para el ID del usuario */
	at24c32_read32( USER_ID_ADDR, &me->id );
	if( me->id == 0x0 || me->id == 0xFFFFFFFF )
		me->id = 0x12345678;
	#ifdef DEBUG_MODE
		ESP_LOGI( TAG, "user:%X", me->id );
	#endif

	/* se obtiene la hora y fecha del RTC */
	ds3231_get_date( &me->rtc );
	at24c32_write8( me->index + 2, &me->rtc.date.date );
	at24c32_write8( me->index + 3, &me->rtc.date.month );
	at24c32_write8( me->index + 4, &me->rtc.date.year );
	#ifdef DEBUG_MODE
		ESP_LOGI( TAG, "date:%02x/%02x/%02x", me->rtc.date.date, me->rtc.date.month, me->rtc.date.year );
	#endif

	/* se configura la alarma 1 del RTC */
	me->rtc.alarm1.seconds = 0x0;
	me->rtc.alarm1.minutes = 0x0;
	me->rtc.alarm1.hours = 0x0;
	me->rtc.alarm1.daydate = 0x0;
	me->rtc.alarm1.mode = HOURS_MINUTES_SECONDS_MATCH;
	ds3231_set_alarm( &me->rtc, ALARM1 );
	me->rtc.alarm_interrupt_mode = ENABLE_ALARM1;
	ds3231_set_alarm_interrupt( &me->rtc );


	/* se inicializala variable para el ID del usuario */
	me->monthly_pulses = 0;
	uint16_t pulses;
	for( uint16_t i = me->index; i >= ( me->index - ( DATA_SIZE * me->monthly_logged_days ) ); i -= DATA_SIZE )
	{
		at24c32_read16( i, &pulses );
		me->monthly_pulses += pulses;
	}
	#ifdef DEBUG_MODE
		ESP_LOGI( TAG, "monthly_pulses:%d", me->monthly_pulses );
	#endif

	/* se inicializan los configuración de interrupciones */
	gpio_config_t gpio_conf;
	gpio_conf.pin_bit_mask = INTERRUPT_PINS_MASK;
	gpio_conf.mode = GPIO_MODE_INPUT;
	gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	gpio_conf.intr_type = GPIO_INTR_NEGEDGE;
	gpio_config( &gpio_conf );

	/* se configuran las interrupciones y los handlers */
	gpio_install_isr_service( 0 );
	gpio_isr_handler_add( GPIO_PULSES, pulses_isr, NULL );
	gpio_isr_handler_add( GPIO_ALARM, alarm_isr, NULL );

	/* se crean las tareas */
	xTaskCreate( pulses_task, "Pulse Count Task", configMINIMAL_STACK_SIZE * 2, ( void * )me, tskIDLE_PRIORITY + 2, &pulses_handle );
	xTaskCreate( alarm_task, "Send Task", configMINIMAL_STACK_SIZE * 2, ( void * )me, tskIDLE_PRIORITY + 2, &alarm_handle );

}

/*==================[internal functions definition]==========================*/

/* interrupt service routine for incoming RTC alarm */
static void IRAM_ATTR alarm_isr( void *pvParameters )
{
	BaseType_t higher_priority_task_woken = pdFALSE;
	vTaskNotifyGiveFromISR( alarm_handle, &higher_priority_task_woken );
	portEND_SWITCHING_ISR( higher_priority_task_woken );
}

/* interrupt service routine for incoming digital pulses */
static void IRAM_ATTR pulses_isr( void *pvParameters )
{
	BaseType_t higher_priority_task_woken = pdFALSE;
	vTaskNotifyGiveFromISR( pulses_handle, &higher_priority_task_woken );
	portEND_SWITCHING_ISR( higher_priority_task_woken );
}

static void pulses_task( void * pvParameters )
{
	uint32_t event_to_process;
	data_logger_t * data_logger = ( data_logger_t * )pvParameters;

	for( ;; )
	{
		/* se espera la notificación del isr de los pulsos */
		event_to_process = ulTaskNotifyTake( pdTRUE, portMAX_DELAY );

		if( event_to_process != 0 )
		{
			/* se aumenta en 1 el conteo de pulsos y se guarda en la EEPROM*/
			data_logger->daily_pulses++;
			data_logger->monthly_pulses++;
			at24c32_write16( data_logger->index, &data_logger->daily_pulses );

			ESP_LOGI( TAG, "[0x%X]=%d", data_logger->index, data_logger->daily_pulses );
			ESP_LOGI( TAG, "pulses_monthly=%d", data_logger->monthly_pulses );

			FILE* f = NULL;
			f = fopen( "/spiffs/pulses.txt", "w" );
			if( f != 0 )
			{
				fprintf( f, "%d", data_logger->daily_pulses );
				fclose( f );
			}
		}
	}
}

static void alarm_task( void * pvParameters )
{
	uint32_t event_to_process;
	data_logger_t * data_logger = ( data_logger_t * )pvParameters;

	for( ;; )
	{
		/* se espera la notificación del isr de la alarma */
		event_to_process = ulTaskNotifyTake( pdTRUE, portMAX_DELAY );

		if( event_to_process != 0 )
		{
			ESP_LOGI( TAG, "Alarm!" );

			// seccion critica
			/* se constuye el paquete a encolar */
			data_logger->transmission.packet.pulses = data_logger->daily_pulses;
			data_logger->transmission.packet.id = data_logger->id;
			// seccion critica
			xQueueSend( data_logger->transmission.queue, &data_logger->transmission, 0 );

			/* se aumenta en 1 el conteo de dias monitoreados y se guarda en la eeprom*/
			data_logger->total_logged_days++;
			at24c32_write16( TOTAL_LOGGED_DAYS_ADDR, &data_logger->total_logged_days );
			ESP_LOGI( TAG, "total:%d", data_logger->total_logged_days );

			/* se aumenta en 1 el conteo de dias monitoreados y se guarda en la eeprom*/
			data_logger->monthly_logged_days++;
			at24c32_write16( MONTHLY_LOGGED_DAYS_ADDR, &data_logger->monthly_logged_days );
			ESP_LOGI( TAG, "monthly:%d", data_logger->monthly_logged_days );

			/* se aumenta el indice la cantidad del tamaño de los datos y se guarda en la eeprom */
			data_logger->index += DATA_SIZE;
			if( data_logger->index > ( 0xFFF - DATA_SIZE ) )
				data_logger->index = BASE_INDEX;
			at24c32_write16( CURRENT_INDEX_ADDR, &data_logger->index );

			/* se pone en 0 el conteo de pulsos y se guarda en la memoria eeprom */
			data_logger->daily_pulses = 0;
			at24c32_write16( data_logger->index, &data_logger->daily_pulses );

			/* se obtiene la nueva fecha y se guarda en la eeprom*/
			uint8_t current_month = data_logger->rtc.date.month;
			ds3231_get_date( &data_logger->rtc );
			at24c32_write8( data_logger->index + 2, &data_logger->rtc.date.date );
			at24c32_write8( data_logger->index + 3, &data_logger->rtc.date.month );
			at24c32_write8( data_logger->index + 4, &data_logger->rtc.date.year );
			ESP_LOGI( TAG, "current:%02x new:%02x", current_month, data_logger->rtc.date.month );
			if( current_month != data_logger->rtc.date.month )
			{
				data_logger->monthly_logged_days = 0;
				at24c32_write16( MONTHLY_LOGGED_DAYS_ADDR, &data_logger->monthly_logged_days );
				data_logger->monthly_pulses = 0;
			}

			/* se borra el flag de la alarma */
			ESP_LOGI( TAG, "clear" );
			ds3231_clear_alarm_flag( 1 );
		}
	}
}


/*==================[end of file]============================================*/
