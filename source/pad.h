/* pad.h -- button bit masks used by the input tables (same values libnx used) */

#ifndef __PAD_H__
#define __PAD_H__

#include <stdint.h>

typedef uint64_t u64;

#define HidNpadButton_A      (1ULL << 0)
#define HidNpadButton_B      (1ULL << 1)
#define HidNpadButton_X      (1ULL << 2)
#define HidNpadButton_Y      (1ULL << 3)
#define HidNpadButton_StickL (1ULL << 4)
#define HidNpadButton_StickR (1ULL << 5)
#define HidNpadButton_L      (1ULL << 6)
#define HidNpadButton_R      (1ULL << 7)
#define HidNpadButton_ZL     (1ULL << 8)
#define HidNpadButton_ZR     (1ULL << 9)
#define HidNpadButton_Plus   (1ULL << 10)
#define HidNpadButton_Minus  (1ULL << 11)
#define HidNpadButton_Left   (1ULL << 12)
#define HidNpadButton_Up     (1ULL << 13)
#define HidNpadButton_Right  (1ULL << 14)
#define HidNpadButton_Down   (1ULL << 15)

#endif
