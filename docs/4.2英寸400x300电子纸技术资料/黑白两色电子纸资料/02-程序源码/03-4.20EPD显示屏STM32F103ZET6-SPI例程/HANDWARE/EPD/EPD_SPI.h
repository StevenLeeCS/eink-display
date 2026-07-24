#ifndef _EPD_SPI_H_
#define _EPD_SPI_H_

#include "sys.h"

#define EPD_SCK_GPIO_CLK						RCC_APB2Periph_GPIOG 	
#define EPD_SCK_GPIO_PORT						GPIOG   	
#define EPD_SCK_GPIO_PIN						GPIO_Pin_12

#define EPD_MOSI_GPIO_CLK						RCC_APB2Periph_GPIOD 	
#define EPD_MOSI_GPIO_PORT					GPIOD   	
#define EPD_MOSI_GPIO_PIN						GPIO_Pin_5

#define EPD_RES_GPIO_CLK						RCC_APB2Periph_GPIOD 	
#define EPD_RES_GPIO_PORT						GPIOD   	
#define EPD_RES_GPIO_PIN						GPIO_Pin_4

#define EPD_DC_GPIO_CLK							RCC_APB2Periph_GPIOD 	
#define EPD_DC_GPIO_PORT						GPIOD   	
#define EPD_DC_GPIO_PIN							GPIO_Pin_15

#define EPD_CS_GPIO_CLK							RCC_APB2Periph_GPIOD	
#define EPD_CS_GPIO_PORT						GPIOD   	
#define EPD_CS_GPIO_PIN							GPIO_Pin_1

#define EPD_BUSY_GPIO_CLK						RCC_APB2Periph_GPIOE 	
#define EPD_BUSY_GPIO_PORT					GPIOE   	
#define EPD_BUSY_GPIO_PIN						GPIO_Pin_8


#define EPD_SCK_Set()								GPIO_SetBits(EPD_SCK_GPIO_PORT,EPD_SCK_GPIO_PIN)
#define EPD_SCK_Clr()								GPIO_ResetBits(EPD_SCK_GPIO_PORT,EPD_SCK_GPIO_PIN)

#define EPD_MOSI_Set()							GPIO_SetBits(EPD_MOSI_GPIO_PORT,EPD_MOSI_GPIO_PIN)
#define EPD_MOSI_Clr()							GPIO_ResetBits(EPD_MOSI_GPIO_PORT,EPD_MOSI_GPIO_PIN)

#define EPD_RES_Set()								GPIO_SetBits(EPD_RES_GPIO_PORT,EPD_RES_GPIO_PIN)
#define EPD_RES_Clr()								GPIO_ResetBits(EPD_RES_GPIO_PORT,EPD_RES_GPIO_PIN)

#define EPD_DC_Set()								GPIO_SetBits(EPD_DC_GPIO_PORT,EPD_DC_GPIO_PIN)
#define EPD_DC_Clr()								GPIO_ResetBits(EPD_DC_GPIO_PORT,EPD_DC_GPIO_PIN)

#define EPD_CS_Set()								GPIO_SetBits(EPD_CS_GPIO_PORT,EPD_CS_GPIO_PIN)
#define EPD_CS_Clr()								GPIO_ResetBits(EPD_CS_GPIO_PORT,EPD_CS_GPIO_PIN)

#define EPD_ReadBUSY								GPIO_ReadInputDataBit(EPD_BUSY_GPIO_PORT,EPD_BUSY_GPIO_PIN)

void EPD_GPIOInit(void);   //初始化GPIO
void EPD_WR_Bus(u8 dat);   //模拟SPI时序
void EPD_WR_REG(u8 reg);   //写入一个命令
void EPD_WR_DATA8(u8 dat); //写入一个字节
#endif






