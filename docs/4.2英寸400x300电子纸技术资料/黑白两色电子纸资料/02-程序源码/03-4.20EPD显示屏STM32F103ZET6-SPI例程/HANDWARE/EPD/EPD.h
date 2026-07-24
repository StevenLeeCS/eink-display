#ifndef _EPD_H_
#define _EPD_H_

#include "EPD_SPI.h"

//Òº¾§·Ö±æÂÊ 
#define EPD_W 400
#define EPD_H 300

#define Fast_Seconds_1_5s 0
#define Fast_Seconds_1_s  1



void EPD_ReadBusy(void);
void EPD_RESET(void);
void EPD_Sleep(void);

void EPD_Update(void);
void EPD_Update_Fast(void);
void EPD_Update_Part(void);
void EPD_Update_4Gray(void);

void EPD_Address_Set(u16 xs,u16 ys,u16 xe,u16 ye);
void EPD_SetCursor(u16 xs,u16 ys);

void EPD_Display(const u8 *Image);
void EPD_Display_Fast(const u8 *Image);
void EPD_Display_Part(u16 x,u16 y,u16 sizex, u16 sizey,const u8 *Image);

void EPD_Init(void);
void EPD_Init_Fast(u8 mode);
void EPD_Init_Part(void);


void EPD_Clear(void);
#endif





