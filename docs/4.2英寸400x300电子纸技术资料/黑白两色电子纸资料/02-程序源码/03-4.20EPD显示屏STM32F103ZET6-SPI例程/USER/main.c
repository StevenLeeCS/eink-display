#include "delay.h"
#include "sys.h"
#include "pic.h"
#include "EPD.h"
#include "EPD_GUI.h"

u8 Image_BW[15000];
int main(void)
{
  float num=12.05;
	delay_init();             //延时函数初始化
	EPD_GPIOInit();    
	EPD_Init();
	EPD_Display(gImage_3);
	EPD_Sleep();		
	EPD_Init_Fast(Fast_Seconds_1_s);
	EPD_Display_Fast(gImage_1);
	EPD_Sleep();
	delay_ms(1000);
	EPD_Clear();
	Paint_NewImage(Image_BW,EPD_W,EPD_H,0,WHITE);
	EPD_Full(WHITE); //清空画布
	EPD_Display_Part(0,0,EPD_W,EPD_H,Image_BW);
  while(1)
	{
		EPD_ShowPicture(16,0,368,198,gImage_2,BLACK);
		EPD_ShowString(68,200,"zhengzhouzhongjingyuan",24,BLACK);
		EPD_ShowString(84,230,"4.2inch",16,BLACK);
		EPD_ShowChinese(140,230,"电子墨水屏断电可显示",16,BLACK);
		EPD_DrawRectangle(20,250,65,295,BLACK,1);
    EPD_DrawRectangle(80,250,125,295,BLACK,0); 
		EPD_DrawCircle(200,270,20,BLACK,1); //Hollow circle.
    EPD_DrawCircle(230,270,20,BLACK,0); 
		EPD_ShowWatch(270,250,num,4,2,48,BLACK);
		num+=0.01;
		EPD_Display_Part(0,0,EPD_W,EPD_H,Image_BW);
		delay_ms(1000);
	}
}

