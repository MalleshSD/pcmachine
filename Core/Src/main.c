/*
 * ================================================================
 *  TOUCH BILLING MACHINE — Bare Metal STM32F103C8T6
 *
 *  Display  : ILI9488 TFT 320x480 → SPI2
 *             PB12=CS PB13=SCK PB15=MOSI PC13=DC PB11=RST PB10=LED
 *  Touch    : XPT2046 → SPI1
 *             PA4=CS  PA5=SCK PA6=MISO PA7=MOSI PB0=IRQ
 *  UART1    : PA9=TX 115200 baud → PC (Python prints invoice)
 *
 *  SCREENS:
 *   0. SPLASH     — shop name + start
 *   1. CUSTOMER   — on-screen keyboard, type customer name
 *   2. ITEM LIST  — 20 items shown as buttons (scroll 2 pages)
 *   3. BILL VIEW  — current bill, qty +/-, delete, print
 *   4. PRINT ANIM — sending animation
 *
 *  ITEM CODES (pre-loaded catalogue):
 *   Each item has: code, name, HSN, unit, rate
 *   Touch the item button to add qty 1, touch again to increment
 *
 *  18% GST = CGST 9% + SGST 9%
 * ================================================================
 */
#include <stdint.h>
#include <string.h>

/* ================================================================
   1. PERIPHERALS
   ================================================================ */
#define PERIPH_BASE  0x40000000UL
#define APB1_BASE   (PERIPH_BASE+0x00000000UL)
#define APB2_BASE   (PERIPH_BASE+0x00010000UL)
#define AHB_BASE    (PERIPH_BASE+0x00020000UL)
#define FLASH_BASE   0x40022000UL
#define RCC_BASE    (AHB_BASE  +0x1000UL)
#define GPIOA_BASE  (APB2_BASE +0x0800UL)
#define GPIOB_BASE  (APB2_BASE +0x0C00UL)
#define GPIOC_BASE  (APB2_BASE +0x1000UL)
#define SPI1_BASE   (APB2_BASE +0x3000UL)
#define SPI2_BASE   (APB1_BASE +0x3800UL)
#define USART1_BASE (APB2_BASE +0x3800UL)

typedef struct{ volatile uint32_t CRL,CRH,IDR,ODR,BSRR,BRR,LCKR; } GPIO_t;
typedef struct{ volatile uint32_t CR1,CR2,SR,DR,CRCPR,RXCRCR,TXCRCR,I2SCFGR,I2SPR; } SPI_t;
typedef struct{ volatile uint32_t SR,DR,BRR2,CR1,CR2,CR3,GTPR; } USART_t;
typedef struct{ volatile uint32_t CR,CFGR,CIR,APB2RSTR,APB1RSTR,AHBENR,APB2ENR,APB1ENR,BDCR,CSR; } RCC_t;
typedef struct{ volatile uint32_t ACR,KEYR,OPTKEYR,SR,CR,AR,R,OBR,WRPR; } FLASH_t;
typedef struct{ volatile uint32_t CTRL,LOAD,VAL,CALIB; } SYSTICK_t;

#define RCC    ((RCC_t    *)RCC_BASE)
#define FLASH  ((FLASH_t  *)FLASH_BASE)
#define GPIOA  ((GPIO_t   *)GPIOA_BASE)
#define GPIOB  ((GPIO_t   *)GPIOB_BASE)
#define GPIOC  ((GPIO_t   *)GPIOC_BASE)
#define SPI1   ((SPI_t    *)SPI1_BASE)
#define SPI2   ((SPI_t    *)SPI2_BASE)
#define USART1 ((USART_t  *)USART1_BASE)
#define SYSTICK ((SYSTICK_t*)0xE000E010UL)

#define GPIO_OUT_PP  0x3U
#define GPIO_AF_PP   0xBU
#define GPIO_IN_FLT  0x4U
#define GPIO_IN_PU   0x8U

static inline void gpio_cfg(GPIO_t *p,uint8_t pin,uint8_t cfg){
    if(pin<8){p->CRL&=~(0xFU<<(pin*4));p->CRL|=((uint32_t)cfg<<(pin*4));}
    else{pin-=8;p->CRH&=~(0xFU<<(pin*4));p->CRH|=((uint32_t)cfg<<(pin*4));}
}
#define PIN_SET(p,n) ((p)->BSRR=(1UL<<(n)))
#define PIN_CLR(p,n) ((p)->BRR =(1UL<<(n)))
#define PIN_RD(p,n)  (((p)->IDR>>(n))&1UL)

/* LCD SPI2 pins */
#define LCD_CS_L()   PIN_CLR(GPIOB,12)
#define LCD_CS_H()   PIN_SET(GPIOB,12)
#define LCD_DC_C()   PIN_CLR(GPIOC,13)
#define LCD_DC_D()   PIN_SET(GPIOC,13)
#define LCD_RST_L()  PIN_CLR(GPIOB,11)
#define LCD_RST_H()  PIN_SET(GPIOB,11)
#define LCD_LED_ON() PIN_SET(GPIOB,10)

/* Touch SPI1 pins */
#define T_CS_L()     PIN_CLR(GPIOA,4)
#define T_CS_H()     PIN_SET(GPIOA,4)
#define T_IRQ()      (!PIN_RD(GPIOB,0))   /* active LOW */

/* ================================================================
   2. CLOCK + SYSTICK
   ================================================================ */
static volatile uint32_t tick=0;
void SysTick_Handler(void){ tick++; }
static void delay_ms(uint32_t ms){ uint32_t s=tick;while((tick-s)<ms); }

static void clock_init(void){
    FLASH->ACR=(FLASH->ACR&~7U)|2U|(1U<<4);
    RCC->CR|=(1U<<16); while(!(RCC->CR&(1U<<17)));
    RCC->CFGR=(1U<<16)|(7U<<18)|(4U<<8);
    RCC->CR|=(1U<<24); while(!(RCC->CR&(1U<<25)));
    RCC->CFGR=(RCC->CFGR&~3U)|2U;
    while((RCC->CFGR&(3U<<2))!=(2U<<2));
    SYSTICK->LOAD=72000UL-1UL; SYSTICK->VAL=0; SYSTICK->CTRL=7U;
}

/* ================================================================
   3. SPI
   ================================================================ */
static uint8_t spi1_xfer(uint8_t d){
    while(!(SPI1->SR&(1U<<1)));
    SPI1->DR=d;
    while(!(SPI1->SR&(1U<<0)));
    return (uint8_t)SPI1->DR;
}
static void spi2_send(uint8_t d){
    while(!(SPI2->SR&(1U<<1)));
    SPI2->DR=d;
    while(SPI2->SR&(1U<<7));
}

/* ================================================================
   4. COLOURS
   ================================================================ */
#define BLACK   0x0000U
#define WHITE   0xFFFFU
#define RED     0xF800U
#define GREEN   0x07E0U
#define BLUE    0x001FU
#define YELLOW  0xFFE0U
#define CYAN    0x07FFU
#define ORANGE  0xFD20U
#define NAVY    0x000FU
#define LTGRAY  0xC618U
#define DKGRAY  0x4208U
#define PURPLE  0x8010U
#define DKGRN   0x03E0U
#define PINK    0xF81FU
#define RGB565(r,g,b) ((uint16_t)(((r&0xF8)<<8)|((g&0xFC)<<3)|((b&0xF8)>>3)))

/* ================================================================
   5. ILI9488 DRIVER
   ================================================================ */
#define SCR_W 320
#define SCR_H 480

static void lcd_cmd(uint8_t c){LCD_DC_C();LCD_CS_L();spi2_send(c);LCD_CS_H();}
static void lcd_dat(uint8_t d){LCD_DC_D();LCD_CS_L();spi2_send(d);LCD_CS_H();}

static void lcd_window(uint16_t x0,uint16_t y0,uint16_t x1,uint16_t y1){
    lcd_cmd(0x2A);lcd_dat(x0>>8);lcd_dat(x0&0xFF);lcd_dat(x1>>8);lcd_dat(x1&0xFF);
    lcd_cmd(0x2B);lcd_dat(y0>>8);lcd_dat(y0&0xFF);lcd_dat(y1>>8);lcd_dat(y1&0xFF);
    lcd_cmd(0x2C);
}
static void lcd_fill(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t c){
    if(!w||!h||x>=SCR_W||y>=SCR_H)return;
    if(x+w>SCR_W) w=SCR_W-x;
    if(y+h>SCR_H) h=SCR_H-y;
    lcd_window(x,y,x+w-1,y+h-1);
    uint8_t r=(c>>11)<<3,g=((c>>5)&0x3F)<<2,b=(c&0x1F)<<3;
    uint32_t n=(uint32_t)w*h;
    LCD_DC_D();LCD_CS_L();
    while(n--){spi2_send(r);spi2_send(g);spi2_send(b);}
    LCD_CS_H();
}
static void lcd_hline(uint16_t x,uint16_t y,uint16_t l,uint16_t c){lcd_fill(x,y,l,1,c);}
static void lcd_vline(uint16_t x,uint16_t y,uint16_t l,uint16_t c){lcd_fill(x,y,1,l,c);}
static void lcd_rect(uint16_t x,uint16_t y,uint16_t w,uint16_t h,uint16_t c){
    lcd_hline(x,y,w,c);lcd_hline(x,y+h-1,w,c);
    lcd_vline(x,y,h,c);lcd_vline(x+w-1,y,h,c);
}
static void lcd_fill_rect_rd(uint16_t x,uint16_t y,uint16_t w,uint16_t h,
                               uint8_t r,uint16_t c){
    /* rounded rectangle fill — just use plain fill, corners not critical */
    lcd_fill(x,y,w,h,c);
}

static void lcd_init(void){
    LCD_RST_H();delay_ms(10);LCD_RST_L();delay_ms(20);LCD_RST_H();delay_ms(150);
    lcd_cmd(0x01);delay_ms(150);
    lcd_cmd(0xC0);lcd_dat(0x17);lcd_dat(0x15);
    lcd_cmd(0xC1);lcd_dat(0x41);
    lcd_cmd(0xC5);lcd_dat(0x00);lcd_dat(0x12);lcd_dat(0x80);
    lcd_cmd(0x3A);lcd_dat(0x66);
    lcd_cmd(0x36);lcd_dat(0x48);
    lcd_cmd(0xE0);
    lcd_dat(0x00);lcd_dat(0x03);lcd_dat(0x09);lcd_dat(0x08);
    lcd_dat(0x16);lcd_dat(0x0A);lcd_dat(0x3F);lcd_dat(0x78);
    lcd_dat(0x4C);lcd_dat(0x09);lcd_dat(0x0A);lcd_dat(0x08);
    lcd_dat(0x16);lcd_dat(0x1A);lcd_dat(0x0F);
    lcd_cmd(0xE1);
    lcd_dat(0x00);lcd_dat(0x16);lcd_dat(0x19);lcd_dat(0x03);
    lcd_dat(0x0F);lcd_dat(0x05);lcd_dat(0x32);lcd_dat(0x45);
    lcd_dat(0x46);lcd_dat(0x04);lcd_dat(0x0E);lcd_dat(0x0D);
    lcd_dat(0x35);lcd_dat(0x37);lcd_dat(0x0F);
    lcd_cmd(0x11);delay_ms(120);
    lcd_cmd(0x29);
    LCD_LED_ON();
}

/* ================================================================
   6. FONT 8x12 — direct ASCII 0x20-0x5A
   ================================================================ */
static const uint8_t FONT[][12]={
{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 20 SPC */
{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00}, /* 21 !   */
{0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 22 "   */
{0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00,0x00}, /* 23 #   */
{0x10,0x7C,0xD0,0x7C,0x16,0xD6,0x7C,0x10,0x00,0x00,0x00,0x00}, /* 24 $   */
{0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 25 %   */
{0x38,0x6C,0x68,0x76,0xDC,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, /* 26 &   */
{0x18,0x18,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 27 '   */
{0x18,0x30,0x60,0x60,0x60,0x30,0x18,0x00,0x00,0x00,0x00,0x00}, /* 28 (   */
{0x60,0x30,0x18,0x18,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, /* 29 )   */
{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00}, /* 2A *   */
{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, /* 2B +   */
{0x00,0x00,0x00,0x00,0x18,0x18,0x10,0x20,0x00,0x00,0x00,0x00}, /* 2C ,   */
{0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 2D -   */
{0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* 2E .   */
{0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00}, /* 2F /   */
{0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 30 0   */
{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00}, /* 31 1   */
{0x7C,0xC6,0x06,0x0C,0x30,0x66,0xFE,0x00,0x00,0x00,0x00,0x00}, /* 32 2   */
{0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 33 3   */
{0x1C,0x3C,0x6C,0xFE,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00,0x00}, /* 34 4   */
{0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 35 5   */
{0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 36 6   */
{0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00}, /* 37 7   */
{0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 38 8   */
{0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00,0x00,0x00,0x00,0x00}, /* 39 9   */
{0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00}, /* 3A :   */
{0x00,0x18,0x18,0x00,0x18,0x18,0x10,0x20,0x00,0x00,0x00,0x00}, /* 3B ;   */
{0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00}, /* 3C <   */
{0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* 3D =   */
{0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00,0x00}, /* 3E >   */
{0x7C,0xC6,0x06,0x0C,0x18,0x00,0x18,0x00,0x00,0x00,0x00,0x00}, /* 3F ?   */
{0x7C,0xC6,0xDE,0xDE,0xC0,0xC0,0x7E,0x00,0x00,0x00,0x00,0x00}, /* 40 @   */
{0x10,0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 41 A   */
{0xFC,0xC6,0xC6,0xFC,0xC6,0xC6,0xFC,0x00,0x00,0x00,0x00,0x00}, /* 42 B   */
{0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 43 C   */
{0xF8,0xCC,0xC6,0xC6,0xC6,0xCC,0xF8,0x00,0x00,0x00,0x00,0x00}, /* 44 D   */
{0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xFE,0x00,0x00,0x00,0x00,0x00}, /* 45 E   */
{0xFE,0xC0,0xC0,0xFC,0xC0,0xC0,0xC0,0x00,0x00,0x00,0x00,0x00}, /* 46 F   */
{0x7C,0xC6,0xC0,0xCE,0xC6,0xC6,0x7A,0x00,0x00,0x00,0x00,0x00}, /* 47 G   */
{0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 48 H   */
{0x7E,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00}, /* 49 I   */
{0x1E,0x06,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 4A J   */
{0xC6,0xCC,0xD8,0xF0,0xD8,0xCC,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 4B K   */
{0xC0,0xC0,0xC0,0xC0,0xC0,0xC0,0xFE,0x00,0x00,0x00,0x00,0x00}, /* 4C L   */
{0x82,0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 4D M   */
{0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 4E N   */
{0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 4F O   */
{0xFC,0xC6,0xC6,0xFC,0xC0,0xC0,0xC0,0x00,0x00,0x00,0x00,0x00}, /* 50 P   */
{0x7C,0xC6,0xC6,0xD6,0xDE,0xCC,0x76,0x00,0x00,0x00,0x00,0x00}, /* 51 Q   */
{0xFC,0xC6,0xC6,0xFC,0xD8,0xCC,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 52 R   */
{0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 53 S   */
{0xFF,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* 54 T   */
{0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00,0x00}, /* 55 U   */
{0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00,0x00}, /* 56 V   */
{0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 57 W   */
{0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00}, /* 58 X   */
{0xC6,0xC6,0x6C,0x38,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00}, /* 59 Y   */
{0xFE,0x06,0x0C,0x18,0x30,0x60,0xFE,0x00,0x00,0x00,0x00,0x00}, /* 5A Z   */
};
#define FW 8
#define FH 12

static void draw_char(uint16_t x,uint16_t y,char c,
                      uint16_t fg,uint16_t bg,uint8_t sc){
    if(c>='a'&&c<='z') c-=32;
    if((uint8_t)c<0x20||(uint8_t)c>0x5A) c=' ';
    const uint8_t *bmp=FONT[(uint8_t)c-0x20];
    for(uint8_t row=0;row<FH;row++){
        uint8_t line=bmp[row];
        for(uint8_t col=0;col<FW;col++){
            uint16_t px=x+(uint16_t)(col*sc);
            uint16_t py=y+(uint16_t)(row*sc);
            if(px<SCR_W&&py<SCR_H)
                lcd_fill(px,py,sc,sc,(line&(0x80>>col))?fg:bg);
        }
    }
}
static void draw_str(uint16_t x,uint16_t y,const char *s,
                     uint16_t fg,uint16_t bg,uint8_t sc){
    uint16_t step=(FW+1)*sc;
    while(*s&&x+FW*sc<=SCR_W){
        draw_char(x,y,*s++,fg,bg,sc); x+=step;
    }
}
static uint16_t str_w(const char *s,uint8_t sc){
    uint16_t n=0; while(*s++){n+=(FW+1)*sc;} return n;
}
static void draw_str_c(uint16_t y,const char *s,uint16_t fg,uint16_t bg,uint8_t sc){
    uint16_t w=str_w(s,sc);
    draw_str(w<SCR_W?(SCR_W-w)/2:0,y,s,fg,bg,sc);
}
static void draw_str_r(uint16_t rx,uint16_t y,const char *s,uint16_t fg,uint16_t bg,uint8_t sc){
    uint16_t w=str_w(s,sc);
    draw_str(rx>w?rx-w:0,y,s,fg,bg,sc);
}

/* Number helpers */
static void u32_str(uint32_t v,char *b){
    if(!v){b[0]='0';b[1]=0;return;}
    char t[12];uint8_t i=0;
    while(v){t[i++]='0'+v%10;v/=10;}
    uint8_t j;
    for(j=0;j<i;j++) b[j]=t[i-1-j];
    b[j]=0;
}
static void price_str(uint32_t paise,char *b){
    uint32_t r=paise/100,p=paise%100;
    u32_str(r,b); uint8_t l=strlen(b);
    b[l]='.';b[l+1]='0'+p/10;b[l+2]='0'+p%10;b[l+3]=0;
}

/* ================================================================
   7. XPT2046 TOUCH DRIVER
   ================================================================ */
/* Calibration — adjust these for your panel */
#define TX_MIN  250U
#define TX_MAX 3800U
#define TY_MIN  250U
#define TY_MAX 3800U

static uint16_t xpt_raw(uint8_t cmd){
    spi1_xfer(cmd);
    uint16_t h=spi1_xfer(0); uint16_t l=spi1_xfer(0);
    return (uint16_t)(((h<<8)|l)>>3)&0x0FFF;
}
static uint8_t touch_get(uint16_t *sx,uint16_t *sy){
    if(!T_IRQ()) return 0;
    T_CS_L();
    uint32_t rx=0,ry=0;
    for(uint8_t i=0;i<8;i++){
        rx+=xpt_raw(0xD0); /* X */
        ry+=xpt_raw(0x90); /* Y */
    }
    T_CS_H();
    if(!T_IRQ()) return 0;
    rx/=8; ry/=8;
    if(rx<TX_MIN) rx=TX_MIN;
    if(rx>TX_MAX) rx=TX_MAX;
    if(ry<TY_MIN) ry=TY_MIN;
    if(ry>TY_MAX) ry=TY_MAX;
    /*
     * Touch calibration for the current ILI9488 orientation.
     *
     * X: direct mapping — LEFT must select LEFT, RIGHT must select RIGHT.
     * Y: inverted mapping — TOP must select TOP, BOTTOM must select BOTTOM.
     *
     * Use SCR_W-1 / SCR_H-1 so the coordinates stay inside
     * 0..319 and 0..479.
     */
    *sx=(uint16_t)(((rx-TX_MIN)*(SCR_W-1U)) /
                   (TX_MAX-TX_MIN));

    *sy=(uint16_t)(SCR_H-1U -
                   ((ry-TY_MIN)*(SCR_H-1U) /
                    (TY_MAX-TY_MIN)));
    return 1;
}

/* Debounced single tap */
static uint8_t tap_get(uint16_t *tx,uint16_t *ty){
    uint16_t x,y;
    if(!touch_get(&x,&y)) return 0;
    delay_ms(30);
    if(!touch_get(tx,ty)) return 0;
    while(T_IRQ()); /* wait release */
    delay_ms(20);
    return 1;
}

/* ================================================================
   8. SHOP CONFIG
   ================================================================ */
#define SHOP_NAME  "BHOOMI STONEX"
#define SHOP_ADDR  "HADADI ROAD, DAVANGERE"
#define SHOP_GSTIN "29BBMPCMALLI2ZE"
#define SHOP_PH    "8431165644"
#define INVOICE_PREFIX "INV"

/* ================================================================
   9. ITEM CATALOGUE — 20 items
      {code, name (max 14 chars), HSN, unit, rate_paise}
   ================================================================ */
typedef struct {
    const char *code;
    const char *name;
    const char *hsn;
    const char *unit;
    uint32_t    rate_paise;  /* per unit × 100 */
} CatItem;

#define N_CAT 20
static const CatItem CAT[N_CAT]={
    {"G01","GRANITE SLAB",   "68022310","SFT", 4600},
    {"G02","GRANITE TILE",   "68022310","SFT", 3800},
    {"G03","GRANITE POLISH", "68022310","SFT", 5200},
    {"M01","MARBLE SLAB",    "25171010","SFT", 5500},
    {"M02","MARBLE TILE",    "25171010","SFT", 4200},
    {"M03","MARBLE FLOORING","25171010","SFT", 6000},
    {"S01","SAND",           "25010010","CFT", 1500},
    {"S02","RIVER SAND",     "25010010","CFT", 2000},
    {"C01","CEMENT BAG",     "25232100","BAG", 40000},
    {"C02","CEMENT BULK",    "25232100","KG",  400},
    {"B01","BRICKS",         "68109900","NOS", 1000},
    {"B02","HOLLOW BLOCK",   "68109900","NOS", 3500},
    {"R01","ROD 8MM",        "72142000","KG",  7500},
    {"R02","ROD 10MM",       "72142000","KG",  7800},
    {"R03","ROD 12MM",       "72142000","KG",  8000},
    {"P01","PAINT 1L",       "32091000","LTR",  95000},
    {"P02","PAINT 4L",       "32091000","LTR",  85000},
    {"T01","FLOOR TILES",    "69072100","SFT",  3200},
    {"T02","WALL TILES",     "69072100","SFT",  2800},
    {"W01","WHITE CEMENT",   "25232100","KG",   1200},
};

/* ================================================================
   10. BILL DATA
   ================================================================ */
#define MAX_BILL 15

typedef struct {
    uint8_t  cat_idx;    /* index into CAT */
    float    qty;        /* quantity (support decimal) */
    uint32_t row_paise;  /* qty × rate */
} BillRow;

static BillRow   bill[MAX_BILL];
static uint8_t   bill_n=0;
static uint32_t  bill_sub=0;      /* subtotal paise */
static uint32_t  bill_no=1;
static char      cust_name[24];   /* customer name */
static uint8_t   bill_page=0;     /* 0 or 1 for item list scroll */

/* ================================================================
   11. UI BUTTON HELPER
   ================================================================ */
typedef struct{ uint16_t x,y,w,h; } Btn;

static void draw_btn(Btn b,uint16_t bg,uint16_t border,
                     const char *label,uint16_t fg,uint8_t sc){
    lcd_fill_rect_rd(b.x,b.y,b.w,b.h,6,bg);
    lcd_rect(b.x,b.y,b.w,b.h,border);
    uint16_t tw=str_w(label,sc);
    uint16_t tx=b.x+(b.w>tw?(b.w-tw)/2:0);
    uint16_t ty=b.y+(b.h>(uint16_t)(FH*sc)?(b.h-FH*sc)/2:2);
    draw_str(tx,ty,label,fg,bg,sc);
}

static uint8_t btn_hit(Btn b,uint16_t tx,uint16_t ty){
    return tx>=b.x&&tx<=b.x+b.w&&ty>=b.y&&ty<=b.y+b.h;
}

/* ================================================================
   12. SCREEN 0 — SPLASH
   ================================================================ */
static void screen_splash(void){
    lcd_fill(0,0,SCR_W,SCR_H,NAVY);

    /* Clean upright portrait splash screen */
    lcd_hline(0,45,SCR_W,CYAN);
    lcd_hline(0,SCR_H-45,SCR_W,CYAN);

    draw_str_c(70, SHOP_NAME, YELLOW, NAVY, 2);
    draw_str_c(105, SHOP_ADDR, CYAN, NAVY, 1);
    draw_str_c(125, SHOP_GSTIN, LTGRAY, NAVY, 1);
    draw_str_c(145, SHOP_PH, WHITE, NAVY, 1);

    draw_str_c(205, "TOUCH BILLING", WHITE, NAVY, 2);
    draw_str_c(235, "MACHINE", CYAN, NAVY, 2);
    draw_str_c(270, "18% GST", YELLOW, NAVY, 1);
    draw_str_c(290, "CGST 9% + SGST 9%", LTGRAY, NAVY, 1);

    Btn start={(SCR_W-200)/2,350,200,60};
    draw_btn(start,GREEN,WHITE,"START",PURPLE,2);
}

/* ================================================================
   13. SCREEN 1 — CUSTOMER NAME (on-screen keyboard)
   ================================================================ */
/* Rendered as individual keys */
static const char KB_R0[]="QWERTYUIOP";
static const char KB_R1[]="ASDFGHJKL";
static const char KB_R2[]="ZXCVBNM";

#define KB_Y0    200   /* top of keyboard */
#define KB_KH     46   /* key height */
#define KB_KW     28   /* key width  */
#define KB_GAP     3   /* gap between keys */

static void draw_keyboard(void){
    /* Row 0 — 10 keys */
    uint8_t n=10;
    uint16_t total=(uint16_t)(n*KB_KW+(n-1)*KB_GAP);
    uint16_t sx=(SCR_W-total)/2;
    for(uint8_t i=0;i<n;i++){
        char lbl[2]={KB_R0[i],0};
        Btn k={(uint16_t)(sx+i*(KB_KW+KB_GAP)),(uint16_t)KB_Y0,KB_KW,KB_KH};
        draw_btn(k,RGB565(50,50,80),CYAN,lbl,WHITE,1);
    }
    /* Row 1 — 9 keys */
    n=9; total=(uint16_t)(n*KB_KW+(n-1)*KB_GAP);
    sx=(SCR_W-total)/2;
    for(uint8_t i=0;i<n;i++){
        char lbl[2]={KB_R1[i],0};
        Btn k={(uint16_t)(sx+i*(KB_KW+KB_GAP)),(uint16_t)(KB_Y0+KB_KH+KB_GAP),KB_KW,KB_KH};
        draw_btn(k,RGB565(50,50,80),CYAN,lbl,WHITE,1);
    }
    /* Row 2 — 7 keys */
    n=7; total=(uint16_t)(n*KB_KW+(n-1)*KB_GAP);
    sx=(SCR_W-total)/2;
    for(uint8_t i=0;i<n;i++){
        char lbl[2]={KB_R2[i],0};
        Btn k={(uint16_t)(sx+i*(KB_KW+KB_GAP)),(uint16_t)(KB_Y0+2*(KB_KH+KB_GAP)),KB_KW,KB_KH};
        draw_btn(k,RGB565(50,50,80),CYAN,lbl,WHITE,1);
    }
    /* Row 3 — SPACE, DEL, OK */
    uint16_t y3=(uint16_t)(KB_Y0+3*(KB_KH+KB_GAP));
    Btn sp={10,y3,120,KB_KH};  draw_btn(sp,DKGRAY,LTGRAY,"SPACE",WHITE,1);
    Btn dl={140,y3,80,KB_KH};  draw_btn(dl,RED,WHITE,"DEL",WHITE,1);
    Btn ok={230,y3,80,KB_KH};  draw_btn(ok,GREEN,WHITE,"OK",NAVY,2);
}

static void screen_customer(void){
    lcd_fill(0,0,SCR_W,SCR_H,NAVY);
    lcd_fill(0,0,SCR_W,50,RGB565(0,30,80));
    draw_str_c(8,"ENTER CUSTOMER NAME",CYAN,RGB565(0,30,80),1);
    draw_str_c(26,"Touch letters below",LTGRAY,RGB565(0,30,80),1);
    lcd_hline(0,50,SCR_W,CYAN);
    /* name display box */
    lcd_fill(8,60,SCR_W-16,50,WHITE);
    lcd_rect(8,60,SCR_W-16,50,CYAN);
    draw_keyboard();
}

static void refresh_name_box(void){
    lcd_fill(10,62,SCR_W-20,46,WHITE);
    draw_str(14,75,cust_name,NAVY,WHITE,2);
    /* cursor */
    uint8_t l=(uint8_t)strlen(cust_name);
    uint16_t cx=(uint16_t)(14+l*(FW+1)*2);
    if(cx<SCR_W-20) lcd_vline(cx,70,26,RED);
}

static char kb_tap(uint16_t tx,uint16_t ty){
    /* Check row 0 */
    uint8_t n=10;
    uint16_t total=(uint16_t)(n*KB_KW+(n-1)*KB_GAP);
    uint16_t sx=(SCR_W-total)/2;
    if(ty>=KB_Y0&&ty<KB_Y0+KB_KH){
        for(uint8_t i=0;i<n;i++){
            uint16_t kx=(uint16_t)(sx+i*(KB_KW+KB_GAP));
            if(tx>=kx&&tx<kx+KB_KW) return KB_R0[i];
        }
    }
    /* Row 1 */
    n=9; total=(uint16_t)(n*KB_KW+(n-1)*KB_GAP); sx=(SCR_W-total)/2;
    uint16_t y1=(uint16_t)(KB_Y0+KB_KH+KB_GAP);
    if(ty>=y1&&ty<y1+KB_KH){
        for(uint8_t i=0;i<n;i++){
            uint16_t kx=(uint16_t)(sx+i*(KB_KW+KB_GAP));
            if(tx>=kx&&tx<kx+KB_KW) return KB_R1[i];
        }
    }
    /* Row 2 */
    n=7; total=(uint16_t)(n*KB_KW+(n-1)*KB_GAP); sx=(SCR_W-total)/2;
    uint16_t y2=(uint16_t)(KB_Y0+2*(KB_KH+KB_GAP));
    if(ty>=y2&&ty<y2+KB_KH){
        for(uint8_t i=0;i<n;i++){
            uint16_t kx=(uint16_t)(sx+i*(KB_KW+KB_GAP));
            if(tx>=kx&&tx<kx+KB_KW) return KB_R2[i];
        }
    }
    /* Row 3 special */
    uint16_t y3=(uint16_t)(KB_Y0+3*(KB_KH+KB_GAP));
    if(ty>=y3&&ty<y3+KB_KH){
        if(tx>=10&&tx<130)  return ' ';  /* SPACE */
        if(tx>=140&&tx<220) return '\b'; /* DEL   */
        if(tx>=230&&tx<310) return '\r'; /* OK    */
    }
    return 0;
}

/* ================================================================
   14. SCREEN 2 — ITEM LIST (10 items per page, 2 pages)
   ================================================================ */
#define ITEM_BTN_H   40
#define ITEM_PER_PG  10

static void screen_items(uint8_t page){
    lcd_fill(0,0,SCR_W,SCR_H,RGB565(15,15,40));

    /* Header */
    lcd_fill(0,0,SCR_W,44,NAVY);
    draw_str_c(4,"SELECT ITEM",CYAN,NAVY,1);
    draw_str_c(20,"Touch item to add to bill",LTGRAY,NAVY,1);

    /* Page indicator */
    char pbuf[12]; u32_str(page+1,pbuf);
    draw_str(270,4,"PG",LTGRAY,NAVY,1);
    draw_str(296,4,pbuf,YELLOW,NAVY,1);

    /* 10 item buttons per page */
    uint8_t start=(uint8_t)(page*ITEM_PER_PG);
    for(uint8_t i=0;i<ITEM_PER_PG&&(start+i)<N_CAT;i++){
        uint8_t idx=start+i;
        uint16_t y=(uint16_t)(44+i*ITEM_BTN_H+2);
        uint16_t bg=(i&1)?RGB565(30,30,60):RGB565(40,40,80);

        lcd_fill(0,y,SCR_W,ITEM_BTN_H-2,bg);
        lcd_rect(0,y,SCR_W,ITEM_BTN_H-2,RGB565(60,60,100));

        /* Code badge */
        lcd_fill(4,y+4,36,ITEM_BTN_H-10,CYAN);
        draw_str_c((uint16_t)(y+12),CAT[idx].code,NAVY,CYAN,1);

        /* Name */
        draw_str(46,y+6, CAT[idx].name,WHITE,bg,1);
        draw_str(46,y+20,CAT[idx].unit,LTGRAY,bg,1);

        /* Price */
        char pb[12]; price_str(CAT[idx].rate_paise,pb);
        draw_str_r(SCR_W-4,y+12,pb,YELLOW,bg,1);

        /* Already in bill? show qty */
        for(uint8_t b=0;b<bill_n;b++){
            if(bill[b].cat_idx==idx){
                char qb[8];
                /* format qty — whole number display */
                u32_str((uint32_t)bill[b].qty,qb);
                draw_str_r(SCR_W-4,y+24,qb,GREEN,bg,1);
                break;
            }
        }
    }

    /* Bottom bar */
    lcd_fill(0,SCR_H-46,SCR_W,46,NAVY);
    lcd_hline(0,SCR_H-46,SCR_W,CYAN);

    /* NAV buttons */
    if(page>0){
        Btn prev={4,SCR_H-42,80,38};
        draw_btn(prev,DKGRAY,LTGRAY,"< PREV",WHITE,1);
    }
    if(page<(N_CAT-1)/ITEM_PER_PG){
        Btn next={90,SCR_H-42,80,38};
        draw_btn(next,DKGRAY,LTGRAY,"NEXT >",WHITE,1);
    }
    Btn bill_btn={180,SCR_H-42,60,38};
    draw_btn(bill_btn,GREEN,WHITE,"BILL",NAVY,1);
    Btn back_btn={248,SCR_H-42,68,38};
    draw_btn(back_btn,RED,WHITE,"CANCEL",WHITE,1);

    /* Mini bill count */
    char bc[8]; u32_str(bill_n,bc);
    draw_str(176,SCR_H-42,"ITEMS:",LTGRAY,NAVY,1);
    /* skip — covered by bill btn */
}

/* ================================================================
   15. SCREEN 3 — BILL VIEW
   ================================================================ */
#define BILL_ROW_H  26
#define BILL_HDR_H  44
#define BILL_LIST_Y 90
#define BILL_FOOT_Y (SCR_H-90)

static void screen_bill(void){
    lcd_fill(0,0,SCR_W,SCR_H,WHITE);

    /* Header */
    lcd_fill(0,0,SCR_W,BILL_HDR_H,NAVY);
    draw_str_c(4, SHOP_NAME,CYAN,NAVY,1);
    draw_str_c(18,"INVOICE",WHITE,NAVY,2);

    /* Customer name */
    lcd_fill(0,BILL_HDR_H,SCR_W,22,RGB565(220,235,255));
    draw_str(4,BILL_HDR_H+5,"BUYER:",NAVY,RGB565(220,235,255),1);
    draw_str(52,BILL_HDR_H+5,cust_name,RED,RGB565(220,235,255),1);
    char bno[8]; u32_str(bill_no,bno);
    draw_str(200,BILL_HDR_H+5,"INV#",NAVY,RGB565(220,235,255),1);
    draw_str(240,BILL_HDR_H+5,bno,RED,RGB565(220,235,255),1);
    lcd_hline(0,BILL_HDR_H+22,SCR_W,NAVY);

    /* Column header */
    lcd_fill(0,BILL_HDR_H+22,SCR_W,16,NAVY);
    draw_str(2,  BILL_HDR_H+24,"ITEM",    WHITE,NAVY,1);
    draw_str(136,BILL_HDR_H+24,"QTY",     WHITE,NAVY,1);
    draw_str(172,BILL_HDR_H+24,"RATE",    WHITE,NAVY,1);
    draw_str(244,BILL_HDR_H+24,"AMOUNT",  WHITE,NAVY,1);

    /* Item rows */
    lcd_fill(0,BILL_LIST_Y,SCR_W,BILL_FOOT_Y-BILL_LIST_Y,WHITE);
    for(uint8_t i=0;i<bill_n&&i<9;i++){
        uint16_t y=(uint16_t)(BILL_LIST_Y+i*BILL_ROW_H);
        uint16_t bg=(i&1)?RGB565(245,248,255):WHITE;
        lcd_fill(0,y,SCR_W,BILL_ROW_H,bg);
        lcd_hline(0,y+BILL_ROW_H-1,SCR_W,LTGRAY);

        BillRow *row=&bill[i];
        draw_str(2,y+7,CAT[row->cat_idx].name,BLACK,bg,1);

        char qb[8]; u32_str((uint32_t)row->qty,qb);
        draw_str(136,y+7,qb,BLACK,bg,1);

        char rb[12]; price_str(CAT[row->cat_idx].rate_paise,rb);
        draw_str(172,y+7,rb,BLUE,bg,1);

        char ab[12]; price_str(row->row_paise,ab);
        draw_str_r(316,y+7,ab,RED,bg,1);
    }

    /* Totals */
    lcd_fill(0,BILL_FOOT_Y,SCR_W,90,RGB565(235,255,235));
    lcd_hline(0,BILL_FOOT_Y,SCR_W,NAVY);

    uint32_t cgst=bill_sub*9/100;
    uint32_t sgst=bill_sub*9/100;
    uint32_t grand=bill_sub+cgst+sgst;

    char sb[12],cb[12],gb[12];
    price_str(bill_sub,sb); price_str(cgst,cb); price_str(grand,gb);

    draw_str(4,BILL_FOOT_Y+4,"SUBTOTAL",BLACK,RGB565(235,255,235),1);
    draw_str_r(316,BILL_FOOT_Y+4,sb,BLACK,RGB565(235,255,235),1);
    draw_str(4,BILL_FOOT_Y+18,"CGST 9%",RGB565(0,100,0),RGB565(235,255,235),1);
    draw_str_r(316,BILL_FOOT_Y+18,cb,RGB565(0,100,0),RGB565(235,255,235),1);
    draw_str(4,BILL_FOOT_Y+32,"SGST 9%",RGB565(0,100,0),RGB565(235,255,235),1);
    draw_str_r(316,BILL_FOOT_Y+32,cb,RGB565(0,100,0),RGB565(235,255,235),1);
    lcd_hline(0,BILL_FOOT_Y+46,SCR_W,NAVY);
    draw_str(4,BILL_FOOT_Y+50,"TOTAL",NAVY,RGB565(235,255,235),2);
    draw_str_r(316,BILL_FOOT_Y+50,gb,RED,RGB565(235,255,235),2);

    /* Footer buttons */
    lcd_fill(0,SCR_H-46,SCR_W,46,NAVY);
    lcd_hline(0,SCR_H-46,SCR_W,CYAN);
    Btn badd={4,  SCR_H-42,70,38}; draw_btn(badd,CYAN,WHITE,"+ ITEM",NAVY,1);
    Btn bdel={80, SCR_H-42,60,38}; draw_btn(bdel,ORANGE,WHITE,"DEL",WHITE,1);
    Btn bcl= {146,SCR_H-42,60,38}; draw_btn(bcl, DKGRAY,WHITE,"CLEAR",WHITE,1);
    Btn bpr= {212,SCR_H-42,100,38};draw_btn(bpr, GREEN,WHITE,"PRINT",NAVY,2);
}

/* ================================================================
   16. QTY INPUT POPUP (appears after tapping an item)
   ================================================================ */
static void draw_qty_popup(uint8_t cat_idx,uint32_t qty_whole,uint32_t qty_dec){
    /* semi-transparent overlay */
    lcd_fill(30,140,260,200,NAVY);
    lcd_rect(30,140,260,200,CYAN);

    draw_str_c(150,CAT[cat_idx].name,CYAN,NAVY,1);
    draw_str_c(168,CAT[cat_idx].unit,LTGRAY,NAVY,1);

    /* Qty display */
    char qbuf[12];
    if(qty_dec==0){
        u32_str(qty_whole,qbuf);
    } else {
        u32_str(qty_whole,qbuf);
        uint8_t l=strlen(qbuf);
        qbuf[l]='.';
        u32_str(qty_dec,qbuf+l+1);
    }
    lcd_fill(80,185,160,36,WHITE);
    lcd_rect(80,185,160,36,CYAN);
    draw_str_c(194,qbuf,NAVY,WHITE,2);

    /* +/- and decimal buttons */
    Btn bm={34, 230,50,40}; draw_btn(bm, RED,WHITE,"-",WHITE,3);
    Btn bp={236,230,50,40}; draw_btn(bp, GREEN,WHITE,"+",WHITE,3);
    Btn bd={110,230,100,40};draw_btn(bd, DKGRAY,LTGRAY,"DECIMAL",WHITE,1);

    /* OK / CANCEL */
    Btn bok ={34, 278,100,44};draw_btn(bok, GREEN,WHITE,"ADD",NAVY,2);
    Btn bcan={186,278,100,44};draw_btn(bcan,RED,WHITE,"CANCEL",WHITE,1);
}

/* ================================================================
   17. PRINT SCREEN
   ================================================================ */
static void screen_print_anim(void){
    lcd_fill(0,0,SCR_W,SCR_H,NAVY);
    draw_str_c(80, "SENDING BILL",WHITE,NAVY,2);
    draw_str_c(110,"TO COMPUTER", CYAN, NAVY,2);
    draw_str_c(150,"PLEASE WAIT...",YELLOW,NAVY,1);
    for(uint8_t i=0;i<30;i++){
        lcd_fill((uint16_t)(5+i*10),190,8,24,GREEN);
        delay_ms(40);
    }
    draw_str_c(250,"BILL SENT!",GREEN,NAVY,3);
    draw_str_c(298,"HP PRINTER",WHITE,NAVY,1);
    delay_ms(1500);
}

/* ================================================================
   18. UART — SEND BILL TO PC
   ================================================================ */
static void uart_str(const char *s){ while(*s){while(!(USART1->SR&(1U<<7)));USART1->DR=(uint8_t)*s++;} }

static void send_bill(void){
    char buf[24];
    uart_str("BILL_START\n");
    uart_str("SHOP:");    uart_str(SHOP_NAME);  uart_str("\n");
    uart_str("ADDR:");    uart_str(SHOP_ADDR);  uart_str("\n");
    uart_str("GSTIN:");   uart_str(SHOP_GSTIN); uart_str("\n");
    uart_str("PHONE:");   uart_str(SHOP_PH);    uart_str("\n");
    uart_str("BUYER:");   uart_str(cust_name);  uart_str("\n");
    uart_str("BILL#:");   u32_str(bill_no,buf); uart_str(buf); uart_str("\n");
    uart_str("GST:18\n");

    for(uint8_t i=0;i<bill_n;i++){
        uart_str("ITEM:");
        uart_str(CAT[bill[i].cat_idx].name); uart_str(",");
        uart_str(CAT[bill[i].cat_idx].hsn);  uart_str(",");
        uart_str(CAT[bill[i].cat_idx].unit); uart_str(",");
        u32_str((uint32_t)bill[i].qty,buf);  uart_str(buf); uart_str(",");
        price_str(CAT[bill[i].cat_idx].rate_paise,buf); uart_str(buf); uart_str(",");
        price_str(bill[i].row_paise,buf); uart_str(buf); uart_str("\n");
    }
    price_str(bill_sub,buf); uart_str("SUBTOTAL:"); uart_str(buf); uart_str("\n");
    uint32_t cgst=bill_sub*9/100;
    price_str(cgst,buf); uart_str("CGST:"); uart_str(buf); uart_str("\n");
    price_str(cgst,buf); uart_str("SGST:"); uart_str(buf); uart_str("\n");
    price_str(bill_sub+cgst*2,buf);
    uart_str("TOTAL:"); uart_str(buf); uart_str("\n");
    uart_str("BILL_END\n");
}

/* ================================================================
   19. PERIPH INIT
   ================================================================ */
static void periph_init(void){
    RCC->APB2ENR|=(1U<<2)|(1U<<3)|(1U<<4)|(1U<<12)|(1U<<14);
    RCC->APB1ENR|=(1U<<14);

    /* SPI2 display */
    GPIOB->BSRR=(1U<<10)|(1U<<11)|(1U<<12);
    GPIOC->BSRR=(1U<<13);
    gpio_cfg(GPIOB,10,GPIO_OUT_PP); gpio_cfg(GPIOB,11,GPIO_OUT_PP);
    gpio_cfg(GPIOB,12,GPIO_OUT_PP); gpio_cfg(GPIOB,13,GPIO_AF_PP);
    gpio_cfg(GPIOB,14,GPIO_IN_FLT); gpio_cfg(GPIOB,15,GPIO_AF_PP);
    gpio_cfg(GPIOC,13,GPIO_OUT_PP);
    SPI2->CR1=(0x2U<<3)|(1U<<2)|(1U<<9)|(1U<<8)|(1U<<6);

    /* SPI1 touch */
    GPIOA->BSRR=(1U<<4);
    gpio_cfg(GPIOA,4,GPIO_OUT_PP); /* T_CS */
    gpio_cfg(GPIOA,5,GPIO_AF_PP);  /* SCK  */
    gpio_cfg(GPIOA,6,GPIO_IN_FLT); /* MISO */
    gpio_cfg(GPIOA,7,GPIO_AF_PP);  /* MOSI */
    gpio_cfg(GPIOB,0,GPIO_IN_PU);  /* IRQ  */
    GPIOB->ODR|=(1U<<0);
    SPI1->CR1=(0x4U<<3)|(1U<<2)|(1U<<9)|(1U<<8)|(1U<<6); /* BR/32 = 2.25MHz */

    /* UART1 PA9 TX 115200 baud */
    gpio_cfg(GPIOA,9, GPIO_AF_PP);
    gpio_cfg(GPIOA,10,GPIO_IN_FLT);
    USART1->BRR2=0x271U;
    USART1->CR1=(1U<<13)|(1U<<3)|(1U<<2);
}

/* ================================================================
   20. MAIN STATE MACHINE
   ================================================================ */
typedef enum{ SCR_SPLASH,SCR_CUSTOMER,SCR_ITEMS,SCR_BILL,SCR_PRINT } Screen;

int main(void){
    clock_init();
    periph_init();
    lcd_init();

    Screen scr=SCR_SPLASH;
    screen_splash();

    /* qty popup state */
    uint8_t  qty_item=0;
    uint32_t qty_whole=1, qty_dec=0;
    uint8_t  qty_popup_active=0;

    cust_name[0]=0;
    memset(bill,0,sizeof(bill));

    uint16_t tx,ty;

    while(1){
        if(!tap_get(&tx,&ty)) continue;

        /* ── SPLASH ── */
        if(scr==SCR_SPLASH){
            scr=SCR_CUSTOMER;
            screen_customer();
            refresh_name_box();
            continue;
        }

        /* ── CUSTOMER NAME ── */
        if(scr==SCR_CUSTOMER){
            char k=kb_tap(tx,ty);
            if(k=='\r'){  /* OK */
                if(strlen(cust_name)==0){
                    /* require at least 1 char */
                    draw_str_c(170,"ENTER NAME FIRST",RED,NAVY,1);
                    delay_ms(800);
                    lcd_fill(0,162,SCR_W,20,NAVY);
                } else {
                    bill_page=0;
                    scr=SCR_ITEMS;
                    screen_items(bill_page);
                }
            } else if(k=='\b'){  /* DEL */
                uint8_t l=strlen(cust_name);
                if(l>0){cust_name[l-1]=0;}
                refresh_name_box();
            } else if(k){
                uint8_t l=strlen(cust_name);
                if(l<22){
                    cust_name[l]=k; cust_name[l+1]=0;
                    refresh_name_box();
                }
            }
            continue;
        }

        /* ── QTY POPUP (must be checked BEFORE ITEM LIST) ── */
        if(scr==SCR_ITEMS && qty_popup_active){
            /* + button */
            Btn bp={236,230,50,40};
            if(btn_hit(bp,tx,ty)){
                if(qty_whole<999) qty_whole++;
                draw_qty_popup(qty_item,qty_whole,qty_dec);
                continue;
            }

            /* - button */
            Btn bm={34,230,50,40};
            if(btn_hit(bm,tx,ty)){
                if(qty_whole>1) qty_whole--;
                draw_qty_popup(qty_item,qty_whole,qty_dec);
                continue;
            }

            /* OK / ADD */
            Btn bok={34,278,100,44};
            if(btn_hit(bok,tx,ty)){
                qty_popup_active=0;

                if(bill_n<MAX_BILL){
                    uint8_t found=0;

                    for(uint8_t b=0;b<bill_n;b++){
                        if(bill[b].cat_idx==qty_item){
                            bill_sub-=bill[b].row_paise;
                            bill[b].qty=(float)qty_whole;
                            bill[b].row_paise=(uint32_t)qty_whole *
                                             CAT[qty_item].rate_paise;
                            bill_sub+=bill[b].row_paise;
                            found=1;
                            break;
                        }
                    }

                    if(!found){
                        bill[bill_n].cat_idx=qty_item;
                        bill[bill_n].qty=(float)qty_whole;
                        bill[bill_n].row_paise=(uint32_t)qty_whole *
                                               CAT[qty_item].rate_paise;
                        bill_sub+=bill[bill_n].row_paise;
                        bill_n++;
                    }
                }

                screen_items(bill_page);
                continue;
            }

            /* CANCEL */
            Btn bcan={186,278,100,44};
            if(btn_hit(bcan,tx,ty)){
                qty_popup_active=0;
                screen_items(bill_page);
                continue;
            }

            /* Ignore taps outside popup */
            continue;
        }

        /* ── ITEM LIST ── */
        if(scr==SCR_ITEMS){
            /* Bottom navigation */
            if(ty>=SCR_H-46){
                Btn prev={4,SCR_H-42,80,38};
                Btn next={90,SCR_H-42,80,38};
                Btn bill_b={180,SCR_H-42,60,38};
                Btn back={248,SCR_H-42,68,38};

                if(btn_hit(prev,tx,ty)&&bill_page>0){
                    bill_page--;
                    screen_items(bill_page);
                }
                else if(btn_hit(next,tx,ty)&&
                        bill_page<(N_CAT-1)/ITEM_PER_PG){
                    bill_page++;
                    screen_items(bill_page);
                }
                else if(btn_hit(bill_b,tx,ty)){
                    scr=SCR_BILL;
                    screen_bill();
                }
                else if(btn_hit(back,tx,ty)){
                    scr=SCR_SPLASH;
                    screen_splash();
                }
                continue;
            }

            /* Item row tap */
            if(ty>=44 && ty<SCR_H-46){
                uint8_t row_vis=(uint8_t)((ty-44)/ITEM_BTN_H);
                uint8_t idx=(uint8_t)(bill_page*ITEM_PER_PG+row_vis);

                if(idx<N_CAT){
                    qty_item=idx;
                    qty_whole=1;
                    qty_dec=0;
                    qty_popup_active=1;
                    draw_qty_popup(qty_item,qty_whole,qty_dec);
                }
            }
            continue;
        }
        /* ── BILL VIEW ── */
        if(scr==SCR_BILL){
            if(ty>=SCR_H-46){
                Btn badd={4,  SCR_H-42,70,38};
                Btn bdel={80, SCR_H-42,60,38};
                Btn bcl= {146,SCR_H-42,60,38};
                Btn bpr= {212,SCR_H-42,100,38};
                if(btn_hit(badd,tx,ty)){
                    bill_page=0; scr=SCR_ITEMS;
                    screen_items(bill_page);
                } else if(btn_hit(bdel,tx,ty)&&bill_n>0){
                    bill_sub-=bill[bill_n-1].row_paise;
                    bill_n--;
                    screen_bill();
                } else if(btn_hit(bcl,tx,ty)){
                    bill_n=0; bill_sub=0;
                    memset(bill,0,sizeof(bill));
                    screen_bill();
                } else if(btn_hit(bpr,tx,ty)){
                    if(bill_n>0){
                        scr=SCR_PRINT;
                        send_bill();
                        screen_print_anim();
                        /* Reset for next bill */
                        bill_no++;
                        bill_n=0; bill_sub=0;
                        memset(bill,0,sizeof(bill));
                        cust_name[0]=0;
                        scr=SCR_SPLASH;
                        screen_splash();
                    }
                }
            }
            continue;
        }
    }
}
