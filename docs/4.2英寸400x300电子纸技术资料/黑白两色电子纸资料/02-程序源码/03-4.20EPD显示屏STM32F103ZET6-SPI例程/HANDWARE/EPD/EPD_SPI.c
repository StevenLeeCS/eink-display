#include "EPD_SPI.h"

//GPIO 初始化
void EPD_GPIOInit(void)
{
	GPIO_InitTypeDef GPIO_InitStructure;
 	RCC_APB2PeriphClockCmd(EPD_SCK_GPIO_CLK|EPD_MOSI_GPIO_CLK|EPD_RES_GPIO_CLK
												 |EPD_DC_GPIO_CLK|EPD_CS_GPIO_CLK|EPD_BUSY_GPIO_CLK,ENABLE);
	GPIO_InitStructure.GPIO_Pin=EPD_SCK_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(EPD_SCK_GPIO_PORT,&GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin=EPD_MOSI_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(EPD_MOSI_GPIO_PORT,&GPIO_InitStructure);
	
	GPIO_InitStructure.GPIO_Pin=EPD_RES_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(EPD_RES_GPIO_PORT,&GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin=EPD_DC_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(EPD_DC_GPIO_PORT,&GPIO_InitStructure);

	GPIO_InitStructure.GPIO_Pin=EPD_CS_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_Out_PP;
	GPIO_InitStructure.GPIO_Speed=GPIO_Speed_50MHz;
	GPIO_Init(EPD_CS_GPIO_PORT,&GPIO_InitStructure);
	
	GPIO_InitStructure.GPIO_Pin=EPD_BUSY_GPIO_PIN;
	GPIO_InitStructure.GPIO_Mode=GPIO_Mode_IPU;
	GPIO_Init(EPD_BUSY_GPIO_PORT,&GPIO_InitStructure);
	
}


//SPI 写入一个字节
void EPD_WR_Bus(u8 dat)
{
	u8 i;
	EPD_CS_Clr();
	for(i=0;i<8;i++)
	{
	  EPD_SCK_Clr();
		if(dat&0x80)
		{
			EPD_MOSI_Set();
		}
		else
		{
			EPD_MOSI_Clr();
		}
		EPD_SCK_Set();
		dat<<=1;
	}
	EPD_CS_Set();
}

//SPI发送指令
void EPD_WR_REG(u8 reg)
{
	EPD_DC_Clr();
  EPD_WR_Bus(reg);
  EPD_DC_Set();
}

//SPI发送数据
void EPD_WR_DATA8(u8 dat)
{
	EPD_WR_Bus(dat);	
}




