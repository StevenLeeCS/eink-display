//////////////////////////////////////////////////////////////////////////////////	 
//本程序只供学习使用，未经作者许可，不得用于其它任何用途
//中景园电子
//店铺地址：http://shop73023976.taobao.com
//
//  文 件 名   : main.c
//  版 本 号   : v2.0
//  作    者   : ZhaoJian
//  生成日期   : 2023-11-04
//  最近修改   : 
//  功能描述   :演示例程(STM32F407系列)
//              说明: 
//              ----------------------------------------------------------------
//              GND   电源地
//              VCC   3.3v电源
//              SCL   PG12（SCLK）
//              SDA   PD5（MOSI）
//              RES   PD4
//              DC    PD15
//              CS    PD1
//              BUSY  PE8
//              ----------------------------------------------------------------
// 修改历史   :
// 日    期   : 
// 作    者   : ZhaoJian
// 修改内容   : 创建文件
//版权所有，盗版必究。
//Copyright(C) 中景园电子2023-11-04
//All rights reserved
//******************************************************************************/

#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "pic.h"
#include "EPD.h"
#include "EPD_GUI.h"

u8 Image_BW[15000];
u8 Image_R[15000];


int main(void)
{ 
  float num=12.05;
	delay_init(168);		  //初始化延时函数
	EPD_GPIOInit();    
	EPD_Init();
	Paint_NewImage(Image_BW,EPD_W,EPD_H,0,WHITE);
	Paint_NewImage(Image_R,EPD_W,EPD_H,0,WHITE);
	Paint_SelectImage(Image_BW);
	EPD_Full(WHITE);
	Paint_SelectImage(Image_R);
	EPD_Full(WHITE);
	EPD_Display(gImage_3,Image_R);
	EPD_Sleep();		
	delay_ms(1000);
	EPD_Clear();
	EPD_Display(Image_BW,gImage_1);
	EPD_Sleep();		
	delay_ms(1000);
	EPD_Clear();
  while(1)
	{
		Paint_SelectImage(Image_BW);
		EPD_Full(WHITE);
		Paint_SelectImage(Image_R);
		EPD_Full(WHITE);
		EPD_ShowPicture(16,0,368,198,gImage_2,BLACK);
		Paint_SelectImage(Image_BW);
		EPD_ShowString(68,200,"zhengzhouzhongjingyuan",16,BLACK);
		EPD_ShowString(84,230,"4.2inch",16,BLACK);
		EPD_ShowChinese(140,230,"电子墨水屏断电可显示",16,BLACK);
		EPD_DrawRectangle(20,250,65,295,BLACK,1);
    EPD_DrawRectangle(80,250,125,295,BLACK,0); 
		EPD_DrawCircle(200,270,20,BLACK,1); //Hollow circle.
    EPD_DrawCircle(230,270,20,BLACK,0); 
		EPD_ShowWatch(270,250,num,4,2,48,BLACK);
		num+=0.01;
		EPD_Display(Image_BW,Image_R);
		delay_ms(1000);
	}
}

