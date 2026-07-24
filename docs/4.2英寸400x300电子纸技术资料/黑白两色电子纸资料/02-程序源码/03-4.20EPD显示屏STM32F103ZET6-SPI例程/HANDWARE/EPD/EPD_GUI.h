#ifndef _EPD_GUI_H_
#define _EPD_GUI_H_

#include "stm32f10x.h"

typedef struct {
    u8 *Image;
    u16 Width;
    u16 Height;
    u16 WidthMemory;
    u16 HeightMemory;
    u16 Color;
    u16 Rotate;
    u16 WidthByte;
    u16 HeightByte;
} PAINT;
extern PAINT Paint;


#define ROTATE_0            0   //屏幕正向显示
#define ROTATE_90           90  //屏幕旋转90度显示
#define ROTATE_180          180 //屏幕旋转180度显示
#define ROTATE_270          270 //屏幕旋转270度显示



#define WHITE          0xFF   //显示白色
#define BLACK          0x00   //显示黑色

void Paint_NewImage(u8 *image,u16 Width,u16 Height,u16 Rotate,u16 Color); 					 //创建画布控制显示方向
void Paint_SetPixel(u16 Xpoint,u16 Ypoint,u16 Color);
void EPD_Full(u16 Color);
void EPD_DrawLine(u16 Xstart,u16 Ystart,u16 Xend,u16 Yend,u16 Color);
void EPD_DrawRectangle(u16 Xstart,u16 Ystart,u16 Xend,u16 Yend,u16 Color,u8 mode);  //画矩形
void EPD_DrawCircle(u16 X_Center,u16 Y_Center,u16 Radius,u16 Color,u8 mode);        //画圆
void EPD_ShowChar(u16 x,u16 y,u16 chr,u16 size1,u16 color);                         //显示字符
void EPD_ShowString(u16 x,u16 y,u8 *chr,u16 size1,u16 color);                       //显示字符串
void EPD_ShowNum(u16 x,u16 y,u32 num,u16 len,u16 size1,u16 color);                  //显示数字
void EPD_ShowPicture(u16 x,u16 y,u16 sizex,u16 sizey,const u8 BMP[],u16 Color);			//显示图片      
void EPD_ClearWindows(u16 xs,u16 ys,u16 xe,u16 ye,u16 color);
void EPD_ShowFloatNum1(u16 x,u16 y,float num,u8 len,u8 pre,u8 sizey,u8 color);

void EPD_ShowChinese(u16 x,u16 y,u8 *s,u8 sizey,u16 color);//显示汉字串
void EPD_ShowChinese12x12(u16 x,u16 y,u8 *s,u8 sizey,u16 color);//显示单个12x12汉字
void EPD_ShowChinese16x16(u16 x,u16 y,u8 *s,u8 sizey,u16 color);//显示单个16x16汉字
void EPD_ShowChinese24x24(u16 x,u16 y,u8 *s,u8 sizey,u16 color);//显示单个24x24汉字
void EPD_ShowChinese32x32(u16 x,u16 y,u8 *s,u8 sizey,u16 color);//显示单个32x32汉字
void EPD_ShowWatch(u16 x,u16 y,float num,u8 len,u8 pre,u8 sizey,u8 color);//显示秒表

#endif





