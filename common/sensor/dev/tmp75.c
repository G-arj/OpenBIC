#include <stdio.h>
#include "sensor.h"
#include "hal_i2c.h"
#include "pal.h"

uint8_t tmp75_read(uint8_t sensor_num, int *reading)
{
	if (!reading)
		return SENSOR_UNSPECIFIED_ERROR;

	uint8_t retry = 5;
	I2C_MSG msg = { 0 };

	msg.bus = sensor_config[SensorNum_SensorCfg_map[sensor_num]].port;
	msg.slave_addr = sensor_config[SensorNum_SensorCfg_map[sensor_num]].slave_addr;
	msg.tx_len = 1;
	msg.rx_len = 1;
	msg.data[0] = sensor_config[SensorNum_SensorCfg_map[sensor_num]].offset;

	if (i2c_master_read(&msg, retry))
		return SENSOR_FAIL_TO_ACCESS;

	sensor_val *sval = (sensor_val *)reading;
	sval->integer = msg.data[0];
	return SENSOR_READ_SUCCESS;
}

uint8_t tmp75_init(uint8_t sensor_num)
{
	sensor_config[SensorNum_SensorCfg_map[sensor_num]].read = tmp75_read;
	return SENSOR_INIT_SUCCESS;
}
