/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : 点阵贪吃蛇 · MAX7219 + 4 按键（在你原有代码上加的）
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_I2C1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ==================== 【新增】按键 ====================
 * 四个按键放在 PA0 / PA1 / PA2 / PA3 ——
 * 它们就在 PA4（蜂鸣器）上面一排，四个脚挨着，接线最整齐。
 *
 * （原来用的是 PB0/PB1/PB3/PB4。之所以要绕开 PB3/PB4，是因为这两个脚默认是
 *   JTAG 的 JTDO/NJTRST，必须先在代码里关掉 JTAG 才能当普通 IO；换成 PA0~PA3
 *   就不用操这个心了。） */
#define KEY_PORT      GPIOA
#define KEY_UP_PIN    GPIO_PIN_0
#define KEY_DOWN_PIN  GPIO_PIN_1
#define KEY_LEFT_PIN  GPIO_PIN_2
#define KEY_RIGHT_PIN GPIO_PIN_3

/* ==================== 【新增】暂停键（PB0） ====================
 * 单独一个键：按一下暂停，再按一下继续。
 * PB0 就在左排针上 PA7 的下面，接线方便。 */
#define KEY_PAUSE_PORT GPIOB
#define KEY_PAUSE_PIN  GPIO_PIN_0

static uint8_t paused = 0;          /* 1 = 暂停中 */

/* ==================== 【新增】组合键暂停 ====================
 * 万一你还没接第 5 个按键：同时按住【上(PA0) + 右(PA3)】超过 0.5 秒，
 * 也当成"按了暂停键"。松手后才能再触发一次，避免连发。
 * 返回 1 = 这一次触发了暂停/继续。 */
static uint8_t Pause_Combo(uint8_t up_and_down_held)
{
    static uint32_t heldSince = 0;
    static uint8_t  fired = 0;

    if (up_and_down_held) {
        if (heldSince == 0) heldSince = HAL_GetTick();
        if (!fired && ((HAL_GetTick() - heldSince) > 250)) {   /* 250ms = 半秒的一半，按起来更省力 */
            fired = 1;
            return 1;
        }
    } else {
        heldSince = 0;
        fired = 0;
    }
    return 0;
}

/* 暂停键的消抖扫描。返回 1 = "这一次按下的瞬间" */
static uint8_t Pause_Scan(void)
{
    static uint8_t  last    = 1;
    static uint8_t  stable  = 1;
    static uint32_t tChange = 0;

    uint8_t now = (HAL_GPIO_ReadPin(KEY_PAUSE_PORT, KEY_PAUSE_PIN) == GPIO_PIN_RESET) ? 0 : 1;

    if (now != last) { last = now; tChange = HAL_GetTick(); return 0; }

    if ((stable != now) && ((HAL_GetTick() - tChange) >= 20)) {
        stable = now;
        if (now == 0) return 1;     /* 按下那一瞬间算一次 */
    }
    return 0;
}

typedef struct {
    uint8_t  stable;      /* 已确认状态：1=松开，0=按下 */
    uint8_t  last;        /* 上一次采样的电平 */
    uint32_t tChange;     /* 电平变化的时刻 */
    uint8_t  pressed;     /* 本次扫描是否发生"按下"事件 */
} Key_t;

static Key_t keys[4];

static void Key_Scan(void)
{
    static const uint16_t kpin[4] = {KEY_UP_PIN, KEY_DOWN_PIN, KEY_LEFT_PIN, KEY_RIGHT_PIN};

    for (uint8_t i = 0; i < 4; i++) {
        /* 按下 = 低电平(0)，松开 = 高电平(1) */
        uint8_t now = (HAL_GPIO_ReadPin(KEY_PORT, kpin[i]) == GPIO_PIN_RESET) ? 0 : 1;

        keys[i].pressed = 0;

        if (now != keys[i].last) {          /* 电平变了：重新计时 */
            keys[i].last    = now;
            keys[i].tChange = HAL_GetTick();
            continue;
        }

        /* 连续 20ms 不变，才承认状态真的变了 */
        if ((keys[i].stable != now) && ((HAL_GetTick() - keys[i].tChange) >= 20)) {
            keys[i].stable = now;
            if (now == 0) keys[i].pressed = 1;   /* 只有"按下"那一瞬间算一次动作 */
        }
    }
}
/* ==================== 【新增】按键结束 ==================== */

/* ==================== MAX7219 驱动（HAL 版） ==================== */
#define MAX_PORT      GPIOA
#define MAX_DIN_PIN   GPIO_PIN_7      /* PA7 */
#define MAX_CS_PIN    GPIO_PIN_6      /* PA6 —— 和 PA5/PA7 挨着的三个针，插起来整齐 */
#define MAX_CLK_PIN   GPIO_PIN_5      /* PA5 */

#define SNAKE_MAX  64

/* 蛇的速度：每走一步等多少毫秒（数字越大越慢）。
   默认 400（每秒 2.5 步）。
   ★ 现在它是【变量】：暂停的时候按"上"加速、按"下"减速，可以现场调。 */
static uint16_t frame_ms = 400;
#define SPEED_MIN   100         /* 最快：每秒 10 步 */
#define SPEED_MAX   900         /* 最慢：约每秒 1.1 步 */
#define SPEED_STEP   20         /* 每按一次调整 20ms */

static uint8_t frame[8];           /* 显存：frame[r] 的 bit c = 1 表示 (r,c) 亮 */

static void MAX_Write(uint8_t reg, uint8_t data)
{
    uint16_t word = (uint16_t)((reg << 8) | data);

    HAL_GPIO_WritePin(MAX_PORT, MAX_CS_PIN, GPIO_PIN_RESET);   /* CS 拉低 */

    for (int8_t i = 15; i >= 0; i--) {
        if (word & (1U << i))
            HAL_GPIO_WritePin(MAX_PORT, MAX_DIN_PIN, GPIO_PIN_SET);
        else
            HAL_GPIO_WritePin(MAX_PORT, MAX_DIN_PIN, GPIO_PIN_RESET);

        HAL_GPIO_WritePin(MAX_PORT, MAX_CLK_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(MAX_PORT, MAX_CLK_PIN, GPIO_PIN_SET);   /* 上升沿移入一位 */
    }

    HAL_GPIO_WritePin(MAX_PORT, MAX_CS_PIN, GPIO_PIN_SET);     /* CS 拉高，数据生效 */
}

static void MAX_Init(void)
{
    MAX_Write(0x0C, 0x01);   /* 退出关断模式，正常显示 */
    MAX_Write(0x09, 0x00);   /* 不译码（点阵模式） */
    MAX_Write(0x0A, 0x04);   /* 亮度 4/15 */
    MAX_Write(0x0B, 0x07);   /* 扫描全部 8 行 */
    MAX_Write(0x0F, 0x00);   /* 关测试模式 */
    for (uint8_t r = 1; r <= 8; r++) MAX_Write(r, 0x00);   /* 清屏 */
}

static void MAX_Show(void)
{
    for (uint8_t r = 0; r < 8; r++) MAX_Write((uint8_t)(r + 1), frame[r]);
}

static void Frame_Clear(void)
{
    for (uint8_t i = 0; i < 8; i++) frame[i] = 0;
}

static void Frame_Set(uint8_t r, uint8_t c, uint8_t on)
{
    if (r >= 8 || c >= 8) return;
    if (on) frame[r] |=  (uint8_t)(1U << c);
    else    frame[r] &= (uint8_t)~(1U << c);
}

/* ==================== 蛇 ==================== */
typedef struct {
    uint8_t x[SNAKE_MAX];
    uint8_t y[SNAKE_MAX];
    uint8_t head, tail, len, dir;    /* dir: 0上 1下 2左 3右 */
} Snake_t;

static Snake_t  snake;
static uint8_t   foodX, foodY;
static uint16_t  score;
static uint8_t   gameOver;
static uint32_t  rndSeed = 0x12345678u;

/* ==================== 【新增】方向缓冲 ==================== */
static uint8_t   g_nextDir  = 3;     /* 缓冲方向，每帧只用一次 */
static uint8_t   g_dirFresh = 0;     /* 本帧是否已经改过方向 */

/* 请求转向：一帧只收一次，且不许 180 度掉头 */
static void Game_RequestDir(uint8_t d)
{
    if (g_dirFresh) return;              /* 一帧只接受一次，防止连按叠加 */
    if ((d ^ snake.dir) == 1) return;    /* 0/1 是上下，2/3 是左右；异或为 1 = 正好相反 */
    g_nextDir  = d;
    g_dirFresh = 1;
}
/* ==================== 【新增】方向缓冲结束 ==================== */

static uint8_t Rnd8(void)
{
    /* 用 xorshift32 做随机数：比原来的线性同余法"低几位"质量好得多，
       不会老是连着抽到同一个格子。返回 0~7。 */
    rndSeed ^= (rndSeed << 13);
    rndSeed ^= (rndSeed >> 17);
    rndSeed ^= (rndSeed << 5);
    return (uint8_t)(rndSeed & 0x07U);
}

static void Snake_GetBody(uint8_t i, uint8_t *px, uint8_t *py)
{
    uint8_t idx = (uint8_t)((snake.head + SNAKE_MAX - i) % SNAKE_MAX);
    *px = snake.x[idx];
    *py = snake.y[idx];
}

static uint8_t Body_Contains(uint8_t x, uint8_t y)
{
    for (uint8_t i = 0; i < snake.len; i++) {
        uint8_t bx, by;
        Snake_GetBody(i, &bx, &by);
        if (bx == x && by == y) return 1;
    }
    return 0;
}

static void Food_Spawn(void)
{
    uint16_t guard = 0;
    do { foodX = Rnd8(); foodY = Rnd8(); guard++; }
    while (Body_Contains(foodX, foodY) && guard < 500);
}

static void Game_Reset(void)
{
    snake.head = 2; snake.tail = 0; snake.len = 3; snake.dir = 3;   /* 朝右 */
    /* ★ 这里以前写反了：头必须放在"前进方向的反方向的最前头"。
       原来是头在最左边却朝右走，第一步就撞上自己的身体 → 一开局就 GAME OVER。
       现在改成：尾巴在最左(0)，头在最右(2)，朝右走，第一步走到 (3,3)，安全。 */
    snake.x[0] = 0; snake.y[0] = 3;      /* 尾巴 */
    snake.x[1] = 1; snake.y[1] = 3;
    snake.x[2] = 2; snake.y[2] = 3;      /* 蛇头（snake.head 指向这一格） */
    score = 0; gameOver = 0;
    g_nextDir = 3; g_dirFresh = 0;          /* 【新增】重置方向缓冲 */
    Food_Spawn();
}

static void Game_Over(void) { gameOver = 1; }

static void Game_Step(void)
{
    uint8_t hx, hy, nx, ny;

    if (g_dirFresh) { snake.dir = g_nextDir; g_dirFresh = 0; }   /* 【新增】应用这一帧的转向 */

    Snake_GetBody(0, &hx, &hy);
    nx = hx; ny = hy;

    switch (snake.dir) {
        case 0: if (hy == 0) { Game_Over(); return; } ny = (uint8_t)(hy - 1); break;
        case 1: if (hy == 7) { Game_Over(); return; } ny = (uint8_t)(hy + 1); break;
        case 2: if (hx == 0) { Game_Over(); return; } nx = (uint8_t)(hx - 1); break;
        case 3: if (hx == 7) { Game_Over(); return; } nx = (uint8_t)(hx + 1); break;
    }

    /* 咬到自己（尾节这一帧会走开，不算） */
    for (uint8_t i = 0; i + 1 < snake.len; i++) {
        uint8_t bx, by;
        Snake_GetBody(i, &bx, &by);
        if (bx == nx && by == ny) { Game_Over(); return; }
    }

    uint8_t eat = (uint8_t)((nx == foodX) && (ny == foodY));

    snake.head = (uint8_t)((snake.head + 1) % SNAKE_MAX);
    snake.x[snake.head] = nx;
    snake.y[snake.head] = ny;

    if (eat) { snake.len++; score++; Food_Spawn(); }
    else     { snake.tail = (uint8_t)((snake.tail + 1) % SNAKE_MAX); }
}

static void Render(void)
{
    Frame_Clear();
    Frame_Set(foodY, foodX, 1);                        /* 画食物 */
    for (uint8_t i = 0; i < snake.len; i++) {          /* 画蛇身 */
        uint8_t bx, by;
        Snake_GetBody(i, &bx, &by);
        Frame_Set(by, bx, 1);
    }
}

/* ==================== 蜂鸣器模块（MH-FMD，接在 PA4） ====================
 * 这块小模块【自带三极管驱动】，所以不需要你自己接三极管，3 根线就够：
 *     VCC → 3.3V（想更响可以接 5V）
 *     GND → GND（和板子共地）
 *     I/O → PA4
 *
 * ★ 关键：模块丝印写着【低电平触发】—— I/O 拉【低】才响，拉高就不响。
 *   所以这里必须"反着写"：
 *        平时 = 高电平（安静）
 *        要响 = 拉低 durMs 毫秒，然后拉回高
 *
 * 如果你手头是别的蜂鸣器（比如"高电平触发"的，或者无源蜂鸣器），
 * 告诉我一声，我把这两行电平反过来 / 换成方波驱动。
 * ==========================================================================*/
#define BUZZ_PORT   GPIOA
#define BUZZ_PIN    GPIO_PIN_4

static void Buzzer_Init(void)
{
    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_SET);     /* 高电平 = 不响（安静） */
}

/* 响一声：durMs = 响多久(ms)。低电平触发 → 拉低就响。 */
static void Buzzer_Beep(uint16_t durMs)
{
    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_RESET);   /* 拉低 = 响 */
    HAL_Delay(durMs);
    HAL_GPIO_WritePin(BUZZ_PORT, BUZZ_PIN, GPIO_PIN_SET);     /* 拉高 = 停 */
}

/* ==================== OLED 驱动（SSD1306 / I2C1 / PB8=SCL PB9=SDA） ==================== */
/* 屏幕地址：0x78 最常见，少数模块是 0x7A。
   不用你手改——开机的时候程序会自己问一遍，哪块屏答应就用哪个。 */
static uint8_t oledAddr  = 0x78;      /* 实际地址 */
static uint8_t oledFound = 0;         /* 0 = 没找到屏 */

static uint8_t OLED_GRAM[8][128];     /* 1024 字节显存 */

/* ---------- 底层 I2C ---------- */
/* ---------- 底层 I2C ---------- */
static uint16_t oledErrCnt = 0;              /* 统计 I2C 错误次数（调试用） */

/* 问一句"屏在不在"：0x78 答应就用 0x78，0x7A 答应就用 0x7A */
static uint8_t OLED_Probe(void)
{
    if (HAL_I2C_IsDeviceReady(&hi2c1, 0x78, 3, 100) == HAL_OK) { oledAddr = 0x78; return 1; }
    if (HAL_I2C_IsDeviceReady(&hi2c1, 0x7A, 3, 100) == HAL_OK) { oledAddr = 0x7A; return 2; }
    return 0;
}

static void OLED_WrCmd(uint8_t cmd)
{
    if (HAL_I2C_Mem_Write(&hi2c1, oledAddr, 0x00, I2C_MEMADD_SIZE_8BIT,
                          &cmd, 1, 100) != HAL_OK) {
        oledErrCnt++;
    }
}
static void OLED_WrDataBuf(uint8_t *buf, uint16_t len)
{
    if (HAL_I2C_Mem_Write(&hi2c1, oledAddr, 0x40, I2C_MEMADD_SIZE_8BIT,
                          buf, len, 1000) != HAL_OK) {
        oledErrCnt++;
    }
}

/* ---------- 把显存刷到屏幕 ---------- */
static void OLED_Refresh(void)
{
    for (uint8_t page = 0; page < 8; page++) {
        OLED_WrCmd((uint8_t)(0xB0 + page));
        OLED_WrCmd(0x00);
        OLED_WrCmd(0x10);
        OLED_WrDataBuf(OLED_GRAM[page], 128);
    }
}

static void OLED_Clear(void)
{
    for (uint8_t p = 0; p < 8; p++)
        for (uint8_t i = 0; i < 128; i++) OLED_GRAM[p][i] = 0;
}

static void OLED_Init(void)
{
    HAL_Delay(100);
    OLED_WrCmd(0xAE);
    OLED_WrCmd(0x20); OLED_WrCmd(0x00);
    OLED_WrCmd(0xB0);
    OLED_WrCmd(0xC8);
    OLED_WrCmd(0x00);
    OLED_WrCmd(0x10);
    OLED_WrCmd(0x40);
    OLED_WrCmd(0x81); OLED_WrCmd(0xFF);
    OLED_WrCmd(0xA1);
    OLED_WrCmd(0xA6);
    OLED_WrCmd(0xA8); OLED_WrCmd(0x3F);
    OLED_WrCmd(0xA4);
    OLED_WrCmd(0xD3); OLED_WrCmd(0x00);
    OLED_WrCmd(0xD5); OLED_WrCmd(0xF0);
    OLED_WrCmd(0xD9); OLED_WrCmd(0x22);
    OLED_WrCmd(0xDA); OLED_WrCmd(0x12);
    OLED_WrCmd(0xDB); OLED_WrCmd(0x20);
    OLED_WrCmd(0x8D); OLED_WrCmd(0x14);
    OLED_WrCmd(0xAF);
    OLED_Clear();
    OLED_Refresh();
}

/* ---------- 5x7 字库（列优先，bit0 在最上面） ---------- */
static const uint8_t FONT5x7[38][5] = {
    {0x00,0x00,0x00,0x00,0x00},   /*  0 空格 */
    {0x00,0x36,0x36,0x00,0x00},   /*  1 : */
    {0x3E,0x51,0x49,0x45,0x3E},   /*  2 0 */
    {0x00,0x42,0x7F,0x40,0x00},   /*  3 1 */
    {0x42,0x61,0x51,0x49,0x46},   /*  4 2 */
    {0x21,0x41,0x45,0x4B,0x31},   /*  5 3 */
    {0x18,0x14,0x12,0x7F,0x10},   /*  6 4 */
    {0x27,0x45,0x45,0x45,0x39},   /*  7 5 */
    {0x3C,0x4A,0x49,0x49,0x30},   /*  8 6 */
    {0x01,0x71,0x09,0x05,0x03},   /*  9 7 */
    {0x36,0x49,0x49,0x49,0x36},   /* 10 8 */
    {0x06,0x49,0x49,0x29,0x1E},   /* 11 9 */
    {0x7E,0x11,0x11,0x11,0x7E},   /* 12 A */
    {0x7F,0x49,0x49,0x49,0x36},   /* 13 B */
    {0x3E,0x41,0x41,0x41,0x22},   /* 14 C */
    {0x7F,0x41,0x41,0x22,0x1C},   /* 15 D */
    {0x7F,0x49,0x49,0x49,0x41},   /* 16 E */
    {0x7F,0x09,0x09,0x09,0x01},   /* 17 F */
    {0x3E,0x41,0x49,0x49,0x7A},   /* 18 G */
    {0x7F,0x08,0x08,0x08,0x7F},   /* 19 H */
    {0x00,0x41,0x7F,0x41,0x00},   /* 20 I */
    {0x20,0x40,0x41,0x3F,0x01},   /* 21 J */
    {0x7F,0x08,0x14,0x22,0x41},   /* 22 K */
    {0x7F,0x40,0x40,0x40,0x40},   /* 23 L */
    {0x7F,0x02,0x0C,0x02,0x7F},   /* 24 M */
    {0x7F,0x04,0x08,0x10,0x7F},   /* 25 N */
    {0x3E,0x41,0x41,0x41,0x3E},   /* 26 O */
    {0x7F,0x09,0x09,0x09,0x06},   /* 27 P */
    {0x3E,0x41,0x51,0x21,0x5E},   /* 28 Q */
    {0x7F,0x09,0x19,0x29,0x46},   /* 29 R */
    {0x46,0x49,0x49,0x49,0x31},   /* 30 S */
    {0x01,0x01,0x7F,0x01,0x01},   /* 31 T */
    {0x3F,0x40,0x40,0x40,0x3F},   /* 32 U */
    {0x1F,0x20,0x40,0x20,0x1F},   /* 33 V */
    {0x3F,0x40,0x38,0x40,0x3F},   /* 34 W */
    {0x63,0x14,0x08,0x14,0x63},   /* 35 X */
    {0x07,0x08,0x70,0x08,0x07},   /* 36 Y */
    {0x61,0x51,0x49,0x45,0x43},   /* 37 Z */
};

static uint8_t Font_Index(char c)
{
    if (c == ' ') return 0;
    if (c == ':') return 1;
    if (c >= '0' && c <= '9') return (uint8_t)(2 + (c - '0'));
    if (c >= 'A' && c <= 'Z') return (uint8_t)(12 + (c - 'A'));
    if (c >= 'a' && c <= 'z') return (uint8_t)(12 + (c - 'a'));
    return 0;
}

/* 画一个字符。size=1 → 6x8 小字；size=2 → 12x16 大字 */
static void OLED_ShowChar(uint8_t x, uint8_t y, char ch, uint8_t size)
{
    uint8_t idx = Font_Index(ch);
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t col = FONT5x7[idx][i];
        for (uint8_t k = 0; k < size; k++) {
            uint8_t xx = (uint8_t)(x + i * size + k);
            if (xx >= 128) return;
            for (uint8_t b = 0; b < 8; b++) {
                if (col & (1U << b)) {
                    for (uint8_t m = 0; m < size; m++) {
                        uint8_t yy = (uint8_t)(y + b * size + m);
                        if (yy < 64) OLED_GRAM[yy / 8][xx] |= (uint8_t)(1U << (yy % 8));
                    }
                }
            }
        }
    }
}

static void OLED_ShowString(uint8_t x, uint8_t y, const char *str, uint8_t size)
{
    while (*str) {
        OLED_ShowChar(x, y, *str, size);
        x = (uint8_t)(x + 6 * size);
        if (x + 6 * size > 128) { x = 0; y = (uint8_t)(y + 8 * size); }
        str++;
    }
}

static void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len, uint8_t size)
{
    for (uint8_t i = 0; i < len; i++) {
        uint32_t div = 1;
        for (uint8_t k = 0; k < (uint8_t)(len - 1 - i); k++) div *= 10;
        OLED_ShowChar((uint8_t)(x + i * 6 * size), y, (char)('0' + (num / div) % 10), size);
    }
}

/* ==================== 把 8x8 的 frame[] 也画到 OLED 上 ====================
 * 左边 64x64 像素 = 一张 8x8 的地图（每格 8 像素），右边写分数。
 * 这样即使点阵没通，也能在 OLED 上看到蛇在走。 */
static void OLED_Pixel(uint8_t x, uint8_t y, uint8_t on)
{
    if (x >= 128 || y >= 64) return;
    if (on) OLED_GRAM[y >> 3][x] |=  (uint8_t)(1U << (y & 7));
    else    OLED_GRAM[y >> 3][x] &= (uint8_t)~(1U << (y & 7));
}

static void OLED_RectFill(uint8_t x0, uint8_t y0, uint8_t w, uint8_t h)
{
    for (uint8_t x = 0; x < w; x++)
        for (uint8_t y = 0; y < h; y++)
            OLED_Pixel((uint8_t)(x0 + x), (uint8_t)(y0 + y), 1);
}

/* 把这个方块擦成空的（用来把食物那一格"挖空"） */
static void OLED_RectClear(uint8_t x0, uint8_t y0, uint8_t w, uint8_t h)
{
    for (uint8_t x = 0; x < w; x++)
        for (uint8_t y = 0; y < h; y++)
            OLED_Pixel((uint8_t)(x0 + x), (uint8_t)(y0 + y), 0);
}

/* 左边画 8x8 地图，右边写分数和长度 */
static void OLED_Show_Game(void)
{
    OLED_Clear();

    /* 格线 */
    for (uint8_t i = 0; i <= 8; i++) {
        for (uint8_t k = 0; k < 64; k++) {
            OLED_Pixel((uint8_t)(i * 8), k, 1);
            OLED_Pixel(k, (uint8_t)(i * 8), 1);
        }
    }
    /* ★ 蛇：只有【蛇头】用不一样的图案（十字 +），蛇身/蛇尾都是实心方块 */
    for (uint8_t i = 0; i < snake.len; i++) {
        uint8_t bx, by;
        Snake_GetBody(i, &bx, &by);
        uint8_t x = (uint8_t)(bx * 8 + 1);
        uint8_t y = (uint8_t)(by * 8 + 1);

        if (i == 0) {
            /* 蛇头：十字 + （一眼就能看出蛇往哪边走） */
            OLED_RectClear(x, y, 6, 6);
            for (uint8_t k = 0; k < 6; k++) {
                OLED_Pixel((uint8_t)(x + k), (uint8_t)(y + 2), 1);
                OLED_Pixel((uint8_t)(x + k), (uint8_t)(y + 3), 1);
                OLED_Pixel((uint8_t)(x + 2), (uint8_t)(y + k), 1);
                OLED_Pixel((uint8_t)(x + 3), (uint8_t)(y + k), 1);
            }
        } else {
            OLED_RectFill(x, y, 6, 6);                 /* 蛇身 / 蛇尾：实心方块 */
        }
    }

    /* ★ 食物：空心方框（中间镂空）—— 和蛇的实心方块一眼区分 */
    {
        uint8_t fx = (uint8_t)(foodX * 8 + 1);
        uint8_t fy = (uint8_t)(foodY * 8 + 1);
        OLED_RectClear(fx, fy, 6, 6);
        for (uint8_t k = 0; k < 6; k++) {
            OLED_Pixel((uint8_t)(fx + k), fy, 1);
            OLED_Pixel((uint8_t)(fx + k), (uint8_t)(fy + 5), 1);
            OLED_Pixel(fx, (uint8_t)(fy + k), 1);
            OLED_Pixel((uint8_t)(fx + 5), (uint8_t)(fy + k), 1);
        }
    }
    /* 右边分数 */
    OLED_ShowString(72, 0, "SCORE", 1);
    OLED_ShowNum(72, 12, score, 3, 2);
    OLED_ShowString(72, 36, "LEN", 1);
    OLED_ShowNum(72, 48, snake.len, 2, 1);
    OLED_Refresh();
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_I2C1_Init();

  
  /* USER CODE BEGIN 2 */
  MAX_Init();          /* 点阵初始化（清屏、亮度） */
  Buzzer_Init();       /* 蜂鸣器模块：先给高电平（低电平触发 → 安静） */

  oledFound = OLED_Probe();   /* 先问一句"屏在不在"，顺便定下用哪个地址 */
  if (oledFound) OLED_Init(); /* 屏幕上电初始化并清屏 */

  Game_Reset();        /* 蛇回到起点，分数归 0，随机放一个食物 */
  Render();            /* 蛇 + 食物 → frame[] */
  MAX_Show();          /* frame[] → 点阵（如果点阵通了，这里就能看到蛇） */
  if (oledFound) OLED_Show_Game();   /* 同一份画面 → OLED（左边地图、右边分数） */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    uint32_t t0 = HAL_GetTick();

    /* ---------- 第 1 步：这一帧里不停地读按键（每 2ms 读一次） ---------- */
    while (HAL_GetTick() - t0 < frame_ms)
    {
      Key_Scan();                                /* 带 20ms 消抖的按键扫描 */

      if (paused) {
        /* ★ 暂停中：上 = 加速（间隔变小），下 = 减速（间隔变大） */
        if (keys[0].pressed) {
          if (frame_ms > SPEED_MIN + SPEED_STEP) frame_ms -= SPEED_STEP;
          else                                   frame_ms = SPEED_MIN;
          Buzzer_Beep(30);
        }
        if (keys[1].pressed) {
          if (frame_ms < SPEED_MAX - SPEED_STEP) frame_ms += SPEED_STEP;
          else                                   frame_ms = SPEED_MAX;
          Buzzer_Beep(30);
        }
      } else {
        if (keys[0].pressed) Game_RequestDir(0);   /* PA0 = 上 */
        if (keys[1].pressed) Game_RequestDir(1);   /* PA1 = 下 */
      }

      if (keys[2].pressed) Game_RequestDir(2);     /* PA2 = 左 */
      if (keys[3].pressed) Game_RequestDir(3);     /* PA3 = 右 */

      /* 暂停：两种方式都行 ——
         ① 按一下 PB0 那个独立暂停键；
         ② 或者同时按住【上(PA0) + 右(PA3)】半秒（不用额外接线）。 */
      uint8_t up_held    = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_RESET) ? 1 : 0;
      uint8_t right_held = (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_3) == GPIO_PIN_RESET) ? 1 : 0;

      if (Pause_Scan() || Pause_Combo((uint8_t)(up_held && right_held))) {
        paused = (uint8_t)(paused ? 0 : 1);
        Buzzer_Beep(60);                 /* 响一下，让你知道按到了 */
      }
      HAL_Delay(2);
    }

    /* ---------- 暂停中：蛇不动，屏幕上写 PAUSE ---------- */
    if (paused)
    {
      if (oledFound)
      {
        OLED_Clear();
        OLED_ShowString(34, 0, "PAUSE", 2);            /* 大字：暂停中 */
        OLED_ShowString(0, 20, "SPEED:", 1);           /* 当前速度（每步多少毫秒） */
        OLED_ShowNum(42, 20, frame_ms, 3, 1);
        OLED_ShowString(66, 20, "MS", 1);

        /* ★ 速度条：亮着的格子越多 = 越快（一共 10 格，一眼就能看懂） */
        {
            uint8_t lvl = (uint8_t)((SPEED_MAX - frame_ms) / ((SPEED_MAX - SPEED_MIN) / 10));
            if (lvl > 10) lvl = 10;
            for (uint8_t i = 0; i < 10; i++) {
                uint8_t x = (uint8_t)(i * 12 + 2);
                if (i < lvl) OLED_RectFill(x, 32, 10, 8);    /* 这一格亮 */
                else         OLED_RectClear(x, 32, 10, 8);   /* 这一格灭 */
            }
        }

        OLED_ShowString(0, 46, "SCORE:", 1);           /* 当前分数 */
        OLED_ShowNum(42, 46, score, 3, 1);
        OLED_ShowString(0, 56, "UP+FAST DN-SLOW", 1);  /* 上键加速、下键减速 */
        OLED_Refresh();
      }
      continue;                          /* 跳过下面的"走一步 + 画画面"，蛇就停住了 */
    }

    /* ---------- 第 2 步：蛇往前走一步 ---------- */
    uint16_t scoreBefore = score;
    Game_Step();

    if (score != scoreBefore)          /* 这一步吃到了食物 */
    {
      Buzzer_Beep(60);                 /* 响 60ms，当"叮"的一声 */
    }

    /* ---------- 第 3 步：把画面画出来 ---------- */
    Render();                          /* 蛇 + 食物 → 显存 */
    MAX_Show();                        /* 显存 → 8x8 点阵 */
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);   /* 板载小灯每帧翻一次 = "程序在跑"的心跳灯 */
    if (oledFound) OLED_Show_Game();   /* 没插屏也不影响游戏继续跑；插了就画地图+分数 */

    /* ---------- 第 4 步：撞墙或咬到自己 ---------- */
    if (gameOver)
    {
      if (oledFound)
      {
        OLED_Clear();                              /* ★ 先把上一帧画面擦掉，不然会叠在一起 */
        OLED_ShowString(20, 16, "GAME OVER", 2);   /* 大字报 */
        OLED_ShowString(28, 44, "SCORE:", 1);      /* 顺便把本局分数写出来 */
        OLED_ShowNum(76, 44, score, 3, 1);
        OLED_Refresh();
      }
      Buzzer_Beep(400);                /* 长响 400ms："嘟嘟"结束音 */
      HAL_Delay(700);

      Game_Reset();
      Render();
      MAX_Show();
      if (oledFound) OLED_Show_Game();   /* 重开一局：OLED 也刷成新的地图 */
    }
  }              /* ★ 这一行结束 while (1) —— 之前就是少了它，导致下面所有函数都被塞进 main 里 */
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 400000;      /* 400 kHz（以前写成 400，那是 400Hz，慢到刷屏要好几秒） */
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();

  /*Configure GPIO pin : PC13 —— 板载小灯，当"心跳灯"用 */
  GPIO_InitStruct.Pin   = GPIO_PIN_13;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7, GPIO_PIN_RESET);   /* 点阵三根，先给低 */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_SET);                           /* 蜂鸣器模块低电平触发：先给高=安静 */

  /*Configure GPIO pins : PA4(蜂鸣器) PA5(CLK) PA6(CS) PA7(DIN)
    —— 四根线在蓝板左排针上是挨着的四个针（PA4 PA5 PA6 PA7） */
  GPIO_InitStruct.Pin = GPIO_PIN_4|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PA0 PA1 PA2 PA3 —— 4 个方向按键（内部上拉，按下=低电平） */
  GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : PB0 —— 暂停键（同样内部上拉） */
  GPIO_InitStruct.Pin = KEY_PAUSE_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(KEY_PAUSE_PORT, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
