#include "EPD.h"
#include "delay.h"

void EPD_ReadBusy(void)
{
  while(1)
  {
		if(EPD_ReadBUSY==0)
		{
			break;
		}
	}
}

void EPD_RESET(void)
{
	EPD_RES_Set();
	delay_ms(100);
	EPD_RES_Clr();
	delay_ms(10);
	EPD_RES_Set();
	delay_ms(100);
}

void EPD_Sleep(void)
{
	EPD_WR_REG(0x10);
	EPD_WR_DATA8(0x01);
	delay_ms(200);
}


void EPD_Update(void)
{
	EPD_WR_REG(0x22);
	EPD_WR_DATA8(0xF7);
  EPD_WR_REG(0x20);
  EPD_ReadBusy();
}
void EPD_Update_Fast(void)
{
  EPD_WR_REG(0x22);
	EPD_WR_DATA8(0xC7);
  EPD_WR_REG(0x20);
  EPD_ReadBusy();	
}

void EPD_Update_Part(void)
{
  EPD_WR_REG(0x22);
	EPD_WR_DATA8(0xFF);
  EPD_WR_REG(0x20);
  EPD_ReadBusy();	
}


void EPD_Address_Set(u16 xs,u16 ys,u16 xe,u16 ye)
{
	EPD_WR_REG(0x44); // SET_RAM_X_ADDRESS_START_END_POSITION
	EPD_WR_DATA8((xs>>3) & 0xFF);
	EPD_WR_DATA8((xe>>3) & 0xFF);

	EPD_WR_REG(0x45); // SET_RAM_Y_ADDRESS_START_END_POSITION
	EPD_WR_DATA8(ys & 0xFF);
	EPD_WR_DATA8((ys >> 8) & 0xFF);
	EPD_WR_DATA8(ye & 0xFF);
	EPD_WR_DATA8((ye >> 8) & 0xFF);
}


void EPD_SetCursor(u16 xs,u16 ys)
{
	EPD_WR_REG(0x4E); // SET_RAM_X_ADDRESS_COUNTER
	EPD_WR_DATA8(xs & 0xFF);

	EPD_WR_REG(0x4F); // SET_RAM_Y_ADDRESS_COUNTER
	EPD_WR_DATA8(ys & 0xFF);
	EPD_WR_DATA8((ys >> 8) & 0xFF);
}


void EPD_Init(void)
{
	EPD_RESET();
	EPD_ReadBusy();   
	EPD_WR_REG(0x12);   // soft  reset
	EPD_ReadBusy();	
	
	EPD_WR_REG(0x3C); //BorderWavefrom
	EPD_WR_DATA8(0x05);	
	EPD_WR_REG(0x11);	// data  entry  mode
	EPD_WR_DATA8(0x03);		// X-mode   	
	EPD_Address_Set(0,0,EPD_W-1,EPD_H-1);
	EPD_SetCursor(0, 0);
	EPD_ReadBusy();	
}


void EPD_Clear(void)
{
	u16 i,j,Width,Height;
	Width = (EPD_W%8==0)?(EPD_W/8):(EPD_W/8+1);
	Height = EPD_H;
	EPD_Init(); 
	EPD_WR_REG(0x24);
	for (j = 0; j < Height; j++) 
	{
		for (i = 0; i < Width; i++) 
		{
			EPD_WR_DATA8(0xFF);
		}
	}
	EPD_WR_REG(0x26);
	for (j = 0; j < Height; j++) 
	{
		for (i = 0; i < Width; i++) 
		{
			EPD_WR_DATA8(0x00);
		}
	}
	EPD_Update();	
}

void EPD_Display(const u8 *BWImage,const u8 *RImage)
{
	u16 i,j,Width,Height;
	Width = (EPD_W%8==0)?(EPD_W/8):(EPD_W/8+1);
	Height = EPD_H;
	EPD_WR_REG(0x24);
	for (j=0;j<Height;j++) 
	{
		for (i=0;i<Width;i++) 
		{
			EPD_WR_DATA8(BWImage[i+j*Width]);
		}
	}
	EPD_WR_REG(0x26);
	for (j=0;j<Height;j++) 
	{
		for (i=0;i<Width;i++) 
		{
			EPD_WR_DATA8(~RImage[i+j*Width]);
		}
	}
	EPD_Update();	
}

