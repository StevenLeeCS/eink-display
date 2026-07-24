#ifndef _EPD_H_
#define _EPD_H_

#include "EPD_SPI.h"

//Òº¾§·Ö±æÂÊ 
#define EPD_W 400
#define EPD_H 300


void EPD_ReadBusy(void);
void EPD_RESET(void);
void EPD_Sleep(void);
void EPD_Update(void);
void EPD_Address_Set(u16 xs,u16 ys,u16 xe,u16 ye);
void EPD_SetCursor(u16 xs,u16 ys);
void EPD_Display(const u8 *BWImage,const u8 *RImage);
void EPD_Init(void);



void EPD_Clear(void);
#endif





